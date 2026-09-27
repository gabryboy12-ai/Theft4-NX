/**
 * @file diagnostics/gpu_flight_recorder.h
 * @brief Bounded, failure-triggered host GPU event recorder.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

namespace rex::diagnostics::gpu_flight {

inline constexpr std::size_t kProductionCapacity = 65536;
inline constexpr std::size_t kOperationCapacity = 64;

// Every producer field is intentionally generic so low-level backends can log
// without constructing strings. Their stable meanings are:
//   object: caller-defined native or guest object identity.
//   submission: monotonically increasing submission identity, when available.
//   frame: caller-defined submitted/presented frame identity.
//   value0/value1: operation-specific unsigned values.
//   result: signed API/backend result; zero denotes no reported error.
// steady_ns uses std::chrono::steady_clock nanoseconds since that clock's
// implementation-defined epoch. Metadata in the JSONL file supplies the
// steady-clock origin and corresponding Unix timestamps.
struct alignas(8) Event {
  uint64_t sequence = 0;
  uint64_t steady_ns = 0;
  uint64_t thread = 0;
  uint64_t object = 0;
  uint64_t submission = 0;
  uint64_t frame = 0;
  uint64_t value0 = 0;
  uint64_t value1 = 0;
  int32_t result = 0;
  uint16_t operation_size = 0;
  uint8_t operation_truncated = 0;
  uint8_t reserved = 0;
  char operation[kOperationCapacity]{};
};

static_assert(sizeof(Event) == 136);
static_assert(std::is_trivially_copyable_v<Event>);

struct SnapshotInfo {
  std::size_t capacity = 0;
  std::size_t retained_count = 0;
  uint64_t recorded_count = 0;
  uint64_t wrapped_count = 0;
  uint64_t first_retained_sequence = 0;
  bool frozen = false;
  bool dumped = false;
};

// Public as a focused test seam. Runtime callers should use the process-global
// functions below. A nonempty path is created exclusively in the constructor;
// an existing path or any initialization error leaves the recorder disabled.
class Recorder {
 public:
  Recorder(std::size_t capacity, std::string_view output_path);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;

  bool enabled() const noexcept;

  void Record(std::string_view operation, uint64_t object = 0,
              uint64_t submission = 0, uint64_t frame = 0,
              uint64_t value0 = 0, uint64_t value1 = 0,
              int32_t result = 0);
  void Fail(std::string_view operation, int32_t result, uint64_t object = 0,
            uint64_t submission = 0, uint64_t frame = 0);
  void Dump(std::string_view reason);

  SnapshotInfo GetSnapshotInfo() const;
  bool GetRetainedEvent(std::size_t chronological_index, Event* event) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Enabled only when REX_GPU_FLIGHT_TRACE_PATH is present and nonempty. Record
// performs no allocation, formatting, logging, or file I/O after initialization.
bool IsEnabled();
void Record(std::string_view operation, uint64_t object = 0,
            uint64_t submission = 0, uint64_t frame = 0,
            uint64_t value0 = 0, uint64_t value1 = 0, int32_t result = 0);
void Fail(std::string_view operation, int32_t result, uint64_t object = 0,
          uint64_t submission = 0, uint64_t frame = 0);
void Dump(std::string_view reason);

}  // namespace rex::diagnostics::gpu_flight

#if defined(_WIN32)
#define REX_GPU_FLIGHT_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define REX_GPU_FLIGHT_EXPORT __attribute__((visibility("default")))
#else
#define REX_GPU_FLIGHT_EXPORT
#endif

// LLDB/manual debugger entry point. Calling it freezes the process-global ring
// and writes it once with reason "manual".
extern "C" REX_GPU_FLIGHT_EXPORT void rex_gpu_flight_dump();
// First-failure dumps call this only after their JSONL stream has flushed and
// closed successfully. It is a no-op debugger rendezvous, not a trap.
extern "C" REX_GPU_FLIGHT_EXPORT void rex_gpu_flight_failure_captured();

#undef REX_GPU_FLIGHT_EXPORT
