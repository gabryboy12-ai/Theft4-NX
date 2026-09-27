#include <rex/diagnostics/gpu_flight_recorder.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace gpu_flight = rex::diagnostics::gpu_flight;

namespace {

class TempTrace {
 public:
  explicit TempTrace(std::string_view label) {
    static std::atomic<uint64_t> counter = 0;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rex_gpu_flight_" + std::string(label) + "_" +
             std::to_string(tick) + "_" +
             std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
             ".jsonl");
  }

  ~TempTrace() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

  std::string Read() const {
    std::ifstream stream(path_, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
  }

 private:
  std::filesystem::path path_;
};

std::string Operation(const gpu_flight::Event& event) {
  return std::string(event.operation, event.operation_size);
}

}  // namespace

TEST_CASE("GPU flight ring retains bounded events in chronological order",
          "[diagnostics][gpu-flight]") {
  TempTrace trace("retention");
  gpu_flight::Recorder recorder(3, trace.path().string());
  REQUIRE(recorder.enabled());
  REQUIRE(std::filesystem::exists(trace.path()));

  recorder.Record("one", 1);
  recorder.Record("two", 2);
  recorder.Record("three", 3);
  recorder.Record("four", 4);
  recorder.Record("five", 5);

  const gpu_flight::SnapshotInfo info = recorder.GetSnapshotInfo();
  REQUIRE(info.capacity == 3);
  REQUIRE(info.retained_count == 3);
  REQUIRE(info.recorded_count == 5);
  REQUIRE(info.wrapped_count == 2);
  REQUIRE(info.first_retained_sequence == 3);
  REQUIRE_FALSE(info.frozen);
  REQUIRE_FALSE(info.dumped);

  const std::vector<std::string> expected_operations = {"three", "four",
                                                         "five"};
  for (std::size_t index = 0; index < expected_operations.size(); ++index) {
    gpu_flight::Event event{};
    REQUIRE(recorder.GetRetainedEvent(index, &event));
    REQUIRE(event.sequence == index + 3);
    REQUIRE(Operation(event) == expected_operations[index]);
    REQUIRE(event.object == index + 3);
  }

  recorder.Dump("manual retention check");
  const std::string output = trace.Read();
  const std::size_t sequence_three = output.find("\"sequence\":3");
  const std::size_t sequence_four = output.find("\"sequence\":4");
  const std::size_t sequence_five = output.find("\"sequence\":5");
  REQUIRE(sequence_three != std::string::npos);
  REQUIRE(sequence_four > sequence_three);
  REQUIRE(sequence_five > sequence_four);
  REQUIRE(output.find("\"wrapped_count\":2") != std::string::npos);
  REQUIRE(output.find("\"first_retained_sequence\":3") !=
          std::string::npos);
}

TEST_CASE("GPU flight recorder preserves the first failure and escapes JSON",
          "[diagnostics][gpu-flight]") {
  TempTrace trace("failure");
  gpu_flight::Recorder recorder(4, trace.path().string());
  REQUIRE(recorder.enabled());

  recorder.Record("before", 11, 12, 13, 14, 15);
  const std::string first_operation =
      std::string("first\"failure\\line\ncontrol") + char{1} +
      static_cast<char>(0xFF);
  recorder.Fail(first_operation, -7, 21, 22, 23);
  recorder.Record("after", 31);
  recorder.Fail("second failure", -8, 32);

  const gpu_flight::SnapshotInfo info = recorder.GetSnapshotInfo();
  REQUIRE(info.recorded_count == 2);
  REQUIRE(info.retained_count == 2);
  REQUIRE(info.wrapped_count == 0);
  REQUIRE(info.first_retained_sequence == 1);
  REQUIRE(info.frozen);
  REQUIRE(info.dumped);

  gpu_flight::Event failure{};
  REQUIRE(recorder.GetRetainedEvent(1, &failure));
  REQUIRE(Operation(failure) == first_operation);
  REQUIRE(failure.result == -7);
  REQUIRE(failure.object == 21);
  REQUIRE(failure.submission == 22);
  REQUIRE(failure.frame == 23);

  const std::string output = trace.Read();
  REQUIRE(output.find("\"type\":\"metadata\"") != std::string::npos);
  REQUIRE(output.find("\"system_unix_ns\":") != std::string::npos);
  REQUIRE(output.find("\"system_unix_us\":") != std::string::npos);
  REQUIRE(output.find("\"steady_origin_ns\":") != std::string::npos);
  REQUIRE(output.find("\"trigger\":\"failure\"") != std::string::npos);
  REQUIRE(output.find("first\\\"failure\\\\line\\ncontrol\\u0001\\u00FF") !=
          std::string::npos);
  REQUIRE(output.find("\"result\":-7") != std::string::npos);
  REQUIRE(output.find("after") == std::string::npos);
  REQUIRE(output.find("second failure") == std::string::npos);
}

TEST_CASE("GPU flight operations are copied into a fixed bounded record",
          "[diagnostics][gpu-flight]") {
  TempTrace trace("operation_bound");
  gpu_flight::Recorder recorder(2, trace.path().string());
  REQUIRE(recorder.enabled());

  const std::string long_operation(80, 'x');
  recorder.Record(long_operation);
  gpu_flight::Event event{};
  REQUIRE(recorder.GetRetainedEvent(0, &event));
  REQUIRE(event.operation_size == gpu_flight::kOperationCapacity);
  REQUIRE(event.operation_truncated == 1);
  REQUIRE(Operation(event) ==
          std::string(gpu_flight::kOperationCapacity, 'x'));
}

TEST_CASE("GPU flight recorder serializes concurrent producers",
          "[diagnostics][gpu-flight]") {
  TempTrace trace("concurrency");
  gpu_flight::Recorder recorder(1024, trace.path().string());
  REQUIRE(recorder.enabled());

  constexpr uint64_t kThreadCount = 4;
  constexpr uint64_t kEventsPerThread = 128;
  std::vector<std::thread> threads;
  for (uint64_t thread_index = 0; thread_index < kThreadCount;
       ++thread_index) {
    threads.emplace_back([&recorder, thread_index] {
      for (uint64_t event_index = 0; event_index < kEventsPerThread;
           ++event_index) {
        recorder.Record("concurrent", thread_index, 0, 0, event_index);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const gpu_flight::SnapshotInfo info = recorder.GetSnapshotInfo();
  REQUIRE(info.recorded_count == 512);
  REQUIRE(info.retained_count == 512);
  REQUIRE(info.wrapped_count == 0);
  for (std::size_t index = 0; index < info.retained_count; ++index) {
    gpu_flight::Event event{};
    REQUIRE(recorder.GetRetainedEvent(index, &event));
    REQUIRE(event.sequence == index + 1);
    REQUIRE(Operation(event) == "concurrent");
  }
}

TEST_CASE("GPU flight recorder never overwrites an existing output",
          "[diagnostics][gpu-flight]") {
  TempTrace trace("exclusive");
  {
    std::ofstream existing(trace.path(), std::ios::binary);
    existing << "keep-me";
  }

  {
    gpu_flight::Recorder recorder(4, trace.path().string());
    REQUIRE_FALSE(recorder.enabled());
    recorder.Record("ignored");
    recorder.Fail("ignored", -1);
    recorder.Dump("ignored");
  }
  REQUIRE(trace.Read() == "keep-me");
}
