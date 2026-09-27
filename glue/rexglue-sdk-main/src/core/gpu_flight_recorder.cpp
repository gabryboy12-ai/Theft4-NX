#include <rex/diagnostics/gpu_flight_recorder.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rex::diagnostics::gpu_flight {
namespace {

constexpr std::string_view kEnvironmentVariable =
    "REX_GPU_FLIGHT_TRACE_PATH";
constexpr std::string_view kSchema = "rex_gpu_flight_v1";

uint64_t SteadyNanoseconds() noexcept {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return count > 0 ? static_cast<uint64_t>(count) : 0;
}

uint64_t SystemNanoseconds() noexcept {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return count > 0 ? static_cast<uint64_t>(count) : 0;
}

uint64_t SystemMicroseconds() noexcept {
  const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return count > 0 ? static_cast<uint64_t>(count) : 0;
}

uint64_t ThreadIdentity() noexcept {
  return static_cast<uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

FILE* OpenExclusive(const char* path, int* open_error) noexcept {
#if defined(_WIN32)
  const int descriptor =
      _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
            _S_IREAD | _S_IWRITE);
#else
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  const int descriptor = open(path, flags, S_IRUSR | S_IWUSR);
#endif
  if (descriptor < 0) {
    if (open_error) {
      *open_error = errno;
    }
    return nullptr;
  }

#if defined(_WIN32)
  FILE* stream = _fdopen(descriptor, "wb");
#else
  FILE* stream = fdopen(descriptor, "wb");
#endif
  if (stream) {
    return stream;
  }

  const int descriptor_error = errno;
#if defined(_WIN32)
  _close(descriptor);
#else
  close(descriptor);
#endif
  if (open_error) {
    *open_error = descriptor_error;
  }
  return nullptr;
}

void ReportInitializationFailure(std::string_view path,
                                 std::string_view detail) noexcept {
  std::fputs("[gpu-flight] disabled: ", stderr);
  std::fwrite(detail.data(), sizeof(char), detail.size(), stderr);
  std::fputs(" for exclusive trace path '", stderr);
  std::fwrite(path.data(), sizeof(char), path.size(), stderr);
  std::fputs("'\n", stderr);
  std::fflush(stderr);
}

void WriteJsonString(FILE* stream, std::string_view value) noexcept {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::fputc('"', stream);
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        std::fputs("\\\"", stream);
        break;
      case '\\':
        std::fputs("\\\\", stream);
        break;
      case '\b':
        std::fputs("\\b", stream);
        break;
      case '\f':
        std::fputs("\\f", stream);
        break;
      case '\n':
        std::fputs("\\n", stream);
        break;
      case '\r':
        std::fputs("\\r", stream);
        break;
      case '\t':
        std::fputs("\\t", stream);
        break;
      default:
        if (byte >= 0x20 && byte <= 0x7E) {
          std::fputc(byte, stream);
        } else {
          const char escaped[] = {'\\', 'u', '0', '0',
                                  kHex[byte >> 4], kHex[byte & 0x0F]};
          std::fwrite(escaped, sizeof(char), sizeof(escaped), stream);
        }
        break;
    }
  }
  std::fputc('"', stream);
}

void WriteEventObject(FILE* stream, const Event& event) noexcept {
  std::fprintf(stream,
               "{\"sequence\":%" PRIu64 ",\"steady_ns\":%" PRIu64
               ",\"thread\":%" PRIu64 ",\"operation\":",
               event.sequence, event.steady_ns, event.thread);
  WriteJsonString(stream,
                  std::string_view(event.operation, event.operation_size));
  std::fprintf(
      stream,
      ",\"operation_truncated\":%s,\"object\":%" PRIu64
      ",\"submission\":%" PRIu64 ",\"frame\":%" PRIu64
      ",\"value0\":%" PRIu64 ",\"value1\":%" PRIu64
      ",\"result\":%" PRId32 "}",
      event.operation_truncated ? "true" : "false", event.object,
      event.submission, event.frame, event.value0, event.value1, event.result);
}

void CloseStream(FILE** stream) noexcept {
  if (*stream) {
    std::fclose(*stream);
    *stream = nullptr;
  }
}

}  // namespace

struct Recorder::Impl {
  explicit Impl(std::size_t requested_capacity, std::string_view requested_path)
      : capacity(requested_capacity), output_path(requested_path) {
    if (capacity == 0 || output_path.empty()) {
      return;
    }
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Event)) {
      ReportInitializationFailure(output_path, "ring capacity is too large");
      return;
    }

    ring.reset(new (std::nothrow) Event[capacity]);
    if (!ring) {
      ReportInitializationFailure(output_path, "cannot allocate event ring");
      return;
    }

    steady_origin_ns = SteadyNanoseconds();
    system_origin_unix_ns = SystemNanoseconds();
    system_origin_unix_us = SystemMicroseconds();

    int open_error = 0;
    sink = OpenExclusive(output_path.c_str(), &open_error);
    if (!sink) {
      const char* message = std::strerror(open_error);
      ReportInitializationFailure(output_path,
                                  message ? std::string_view(message)
                                          : std::string_view("open failed"));
      ring.reset();
      return;
    }

    WriteMetadata();
    if (std::fflush(sink) != 0 || std::ferror(sink) != 0) {
      ReportInitializationFailure(output_path,
                                  "cannot write startup metadata");
      CloseStream(&sink);
      std::remove(output_path.c_str());
      ring.reset();
      return;
    }
    is_enabled = true;
  }

  ~Impl() { CloseStream(&sink); }

  void WriteMetadata() const noexcept {
    std::fprintf(
        sink,
        "{\"type\":\"metadata\",\"schema\":\"%.*s\","
        "\"system_unix_ns\":%" PRIu64 ",\"system_unix_us\":%" PRIu64
        ",\"steady_origin_ns\":%" PRIu64
        ",\"capacity\":%zu,\"operation_capacity\":%zu,"
        "\"clock_semantics\":\"steady_ns is std::chrono::steady_clock "
        "nanoseconds since its implementation-defined epoch; system_unix_ns "
        "and system_unix_us are startup std::chrono::system_clock samples\","
        "\"thread_semantics\":\"uint64 hash of std::thread::id, stable only "
        "within this process\",\"field_semantics\":{"
        "\"object\":\"caller-defined native or guest object identity\","
        "\"submission\":\"caller-defined monotonically increasing submission "
        "identity, or zero\",\"frame\":\"caller-defined submitted or "
        "presented frame identity, or zero\",\"value0\":\"operation-specific "
        "unsigned value\",\"value1\":\"operation-specific unsigned value\","
        "\"result\":\"signed API or backend result; zero means no reported "
        "error\"},\"wrapped_count_semantics\":\"number of overwritten "
        "records\"}\n",
        static_cast<int>(kSchema.size()), kSchema.data(), system_origin_unix_ns,
        system_origin_unix_us, steady_origin_ns, capacity, kOperationCapacity);
  }

  Event Append(std::string_view operation, uint64_t object, uint64_t submission,
               uint64_t frame, uint64_t value0, uint64_t value1,
               int32_t result) noexcept {
    Event event{};
    event.sequence = next_sequence;
    event.steady_ns = SteadyNanoseconds();
    event.thread = ThreadIdentity();
    event.object = object;
    event.submission = submission;
    event.frame = frame;
    event.value0 = value0;
    event.value1 = value1;
    event.result = result;
    const std::size_t copied_size =
        std::min(operation.size(), kOperationCapacity);
    event.operation_size = static_cast<uint16_t>(copied_size);
    event.operation_truncated = operation.size() > copied_size ? 1 : 0;
    if (copied_size != 0) {
      std::memcpy(event.operation, operation.data(), copied_size);
    }

    ring[next_index] = event;
    ++next_sequence;
    ++recorded_count;
    ++next_index;
    if (next_index == capacity) {
      next_index = 0;
    }
    if (retained_count < capacity) {
      ++retained_count;
    } else {
      ++wrapped_count;
    }
    return event;
  }

  std::size_t OldestIndex() const noexcept {
    return retained_count == capacity ? next_index : 0;
  }

  uint64_t FirstRetainedSequence() const noexcept {
    return retained_count == 0 ? 0 : ring[OldestIndex()].sequence;
  }

  void WriteCause(std::string_view trigger, std::string_view reason,
                  const Event* cause) const noexcept {
    std::fputs("{\"type\":\"cause\",\"trigger\":", sink);
    WriteJsonString(sink, trigger);
    std::fputs(",\"reason\":", sink);
    WriteJsonString(sink, reason);
    std::fputs(",\"event\":", sink);
    if (cause) {
      WriteEventObject(sink, *cause);
    } else {
      std::fputs("null", sink);
    }
    std::fputs("}\n", sink);
  }

  void WriteEvents() const noexcept {
    std::size_t index = OldestIndex();
    for (std::size_t offset = 0; offset < retained_count; ++offset) {
      std::fputs("{\"type\":\"event\",\"event\":", sink);
      WriteEventObject(sink, ring[index]);
      std::fputs("}\n", sink);
      ++index;
      if (index == capacity) {
        index = 0;
      }
    }
  }

  void WriteFooter() const noexcept {
    std::fprintf(sink,
                 "{\"type\":\"footer\",\"recorded_count\":%" PRIu64
                 ",\"retained_count\":%zu,\"wrapped_count\":%" PRIu64
                 ",\"first_retained_sequence\":%" PRIu64 "}\n",
                 recorded_count, retained_count, wrapped_count,
                 FirstRetainedSequence());
  }

  bool DumpFrozen(std::string_view trigger, std::string_view reason,
                  const Event* cause) noexcept {
    dumped = true;
    WriteCause(trigger, reason, cause);
    WriteEvents();
    WriteFooter();

    const bool flush_succeeded =
        std::fflush(sink) == 0 && std::ferror(sink) == 0;
    const int close_result = std::fclose(sink);
    sink = nullptr;
    const bool write_failed = !flush_succeeded || close_result != 0;
    if (write_failed) {
      std::fprintf(stderr, "[gpu-flight] capture write failed for '%s'\n",
                   output_path.c_str());
    } else {
      std::fprintf(stderr, "[gpu-flight] capture written to '%s'\n",
                   output_path.c_str());
    }
    std::fflush(stderr);
    return !write_failed;
  }

  mutable std::mutex mutex;
  std::unique_ptr<Event[]> ring;
  std::size_t capacity = 0;
  std::string output_path;
  FILE* sink = nullptr;
  uint64_t steady_origin_ns = 0;
  uint64_t system_origin_unix_ns = 0;
  uint64_t system_origin_unix_us = 0;
  uint64_t next_sequence = 1;
  uint64_t recorded_count = 0;
  uint64_t wrapped_count = 0;
  std::size_t next_index = 0;
  std::size_t retained_count = 0;
  bool is_enabled = false;
  bool frozen = false;
  bool dumped = false;
};

Recorder::Recorder(std::size_t capacity, std::string_view output_path)
    : impl_(std::make_unique<Impl>(capacity, output_path)) {}

Recorder::~Recorder() = default;

bool Recorder::enabled() const noexcept { return impl_->is_enabled; }

void Recorder::Record(std::string_view operation, uint64_t object,
                      uint64_t submission, uint64_t frame, uint64_t value0,
                      uint64_t value1, int32_t result) {
  if (!impl_->is_enabled) {
    return;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->frozen) {
    return;
  }
  impl_->Append(operation, object, submission, frame, value0, value1, result);
}

void Recorder::Fail(std::string_view operation, int32_t result,
                    uint64_t object, uint64_t submission, uint64_t frame) {
  if (!impl_->is_enabled) {
    return;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->frozen) {
    return;
  }
  const Event cause =
      impl_->Append(operation, object, submission, frame, 0, 0, result);
  impl_->frozen = true;
  if (impl_->DumpFrozen("failure", "first failure", &cause)) {
    rex_gpu_flight_failure_captured();
  }
}

void Recorder::Dump(std::string_view reason) {
  if (!impl_->is_enabled) {
    return;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->frozen) {
    return;
  }
  impl_->frozen = true;
  impl_->DumpFrozen("manual", reason, nullptr);
}

SnapshotInfo Recorder::GetSnapshotInfo() const {
  std::lock_guard lock(impl_->mutex);
  return SnapshotInfo{
      .capacity = impl_->capacity,
      .retained_count = impl_->retained_count,
      .recorded_count = impl_->recorded_count,
      .wrapped_count = impl_->wrapped_count,
      .first_retained_sequence = impl_->FirstRetainedSequence(),
      .frozen = impl_->frozen,
      .dumped = impl_->dumped,
  };
}

bool Recorder::GetRetainedEvent(std::size_t chronological_index,
                                Event* event) const {
  if (!event) {
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  if (chronological_index >= impl_->retained_count) {
    return false;
  }
  std::size_t index = impl_->OldestIndex() + chronological_index;
  if (index >= impl_->capacity) {
    index -= impl_->capacity;
  }
  *event = impl_->ring[index];
  return true;
}

namespace {

Recorder& GlobalRecorder() {
  static Recorder recorder(
      kProductionCapacity, [] {
        const char* configured_path =
            std::getenv(kEnvironmentVariable.data());
        return configured_path ? std::string_view(configured_path)
                               : std::string_view{};
      }());
  return recorder;
}

}  // namespace

bool IsEnabled() { return GlobalRecorder().enabled(); }

void Record(std::string_view operation, uint64_t object, uint64_t submission,
            uint64_t frame, uint64_t value0, uint64_t value1, int32_t result) {
  GlobalRecorder().Record(operation, object, submission, frame, value0, value1,
                          result);
}

void Fail(std::string_view operation, int32_t result, uint64_t object,
          uint64_t submission, uint64_t frame) {
  GlobalRecorder().Fail(operation, result, object, submission, frame);
}

void Dump(std::string_view reason) { GlobalRecorder().Dump(reason); }

}  // namespace rex::diagnostics::gpu_flight

extern "C" void rex_gpu_flight_dump() {
  rex::diagnostics::gpu_flight::Dump("manual");
}

#if defined(_MSC_VER)
#define REX_GPU_FLIGHT_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define REX_GPU_FLIGHT_NOINLINE __attribute__((noinline))
#else
#define REX_GPU_FLIGHT_NOINLINE
#endif

extern "C" REX_GPU_FLIGHT_NOINLINE void rex_gpu_flight_failure_captured() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

#undef REX_GPU_FLIGHT_NOINLINE
