/**
 * @file diagnostics/export_stubs.h
 * @brief Accounting for Xbox exports implemented only as stubs (REX_EXPORT_STUB).
 *
 * A stub export does nothing (or returns a fixed value) and lets the game go
 * on, as on the desktop. Each one is logged on its first call, counted, and
 * listed with its count by LogExportStubSummary. With the cvar
 * nx_fatal_stubs a stub stops the run instead
 * (rex::diagnostics::StopAtMissingFunction).
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::diagnostics {

// One per REX_EXPORT_STUB, a function-local static: constant-initialised, so
// the hot path is one relaxed increment.
struct ExportStubSite {
  const char* name;  // "__imp__<Export>"
  std::atomic<uint64_t> hits{0};
  ExportStubSite* next = nullptr;  // list of the stubs hit, linked on the first hit
};

// Counts a call; on the first one logs the export and the guest LR. With
// nx_fatal_stubs, stops the run (does not return).
void RecordExportStubHit(ExportStubSite& site, uint32_t guest_lr);

// Logs how many distinct stubs were hit and one line per stub with its count,
// most called first. `when` says at which point ("at the stop", "at exit").
void LogExportStubSummary(const char* when);

// ── Implemented exports (REX_EXPORT): Switch, or REX_TRACE_EXPORTS ──────────
//
// Run 11 resumed the game's main thread and then logged nothing: implemented
// exports leave no trace, so the log could not say how far the game got nor
// where each thread was. On NX, REX_EXPORT goes through ExportCallScope:
// the first call of each export is logged (with the guest LR and the thread),
// each export counts its calls, and each host thread records the export it is
// inside (with the entry time) and the last one it left. A snapshot of every
// thread (switch-game) then says "blocked in KeWaitForSingleObject for 40 s".

// One per REX_EXPORT, a function-local static. `calls` is written by every
// thread that calls the export, so it is an atomic (relaxed).
struct ExportCallSite {
  const char* name;  // "__imp__<Export>"
  std::atomic<uint64_t> calls{0};
  ExportCallSite* next = nullptr;  // list of the exports called, linked on the first call
};

// Per host thread, created on its first export call and never freed. Each
// field is written only by its own thread (plain load + store, no
// read-modify-write) and read by the snapshot on another thread (relaxed).
struct ExportThreadTrace {
  std::atomic<const char*> current{nullptr};  // export the thread is inside, or null
  std::atomic<const char*> last{nullptr};     // last export entered
  std::atomic<uint64_t> enter_ticks{0};       // monotonic ticks at the entry of `current`
  std::atomic<uint32_t> guest_lr{0};          // guest LR of the call to `current`/`last`
  std::atomic<uint64_t> calls{0};             // export calls made by this thread
  uint32_t nx_handle = 0;                     // Horizon thread handle (0 elsewhere)
  uint64_t nx_thread_id = 0;                  // Horizon thread id; elsewhere the runtime's
                                              // thread id (the XThread handle)
  ExportThreadTrace* next = nullptr;
};

// Entry and exit of an export; Enter returns what Leave needs to restore
// (exports may nest through guest callbacks).
// On the first call of an export, *first is set and Leave logs the value r3
// holds after it ("export first result"), so two platforms' traces can be
// compared on results too (tools/compare_export_traces.py).
const char* ExportEnter(ExportCallSite& site, uint32_t guest_lr, bool* first);
void ExportLeave(const char* previous, ExportCallSite* first_site, uint64_t r3);

class ExportCallScope {
 public:
  ExportCallScope(ExportCallSite& site, uint32_t guest_lr, const uint64_t* r3)
      : site_(site), r3_(r3), previous_(ExportEnter(site, guest_lr, &first_)) {}
  ~ExportCallScope() { ExportLeave(previous_, first_ ? &site_ : nullptr, *r3_); }
  ExportCallScope(const ExportCallScope&) = delete;
  ExportCallScope& operator=(const ExportCallScope&) = delete;

 private:
  ExportCallSite& site_;
  const uint64_t* r3_;
  bool first_ = false;
  const char* previous_;
};

// A copy of one thread's trace (names without the "__imp__" prefix).
struct ExportThreadSnapshot {
  const char* current = nullptr;
  const char* last = nullptr;
  double seconds_in_current = 0;
  uint32_t guest_lr = 0;
  uint64_t calls = 0;
  uint32_t nx_handle = 0;
  uint64_t nx_thread_id = 0;
};

// Copies every thread's trace (takes a mutex: call it before pausing any
// thread). Returns the number of threads written, at most `capacity`.
size_t SnapshotExportThreads(ExportThreadSnapshot* out, size_t capacity);

// Totals of the implemented exports: distinct exports called, calls.
void GetExportCallTotals(size_t* distinct, uint64_t* calls);

// Logs every export called with its count, most called first.
void LogExportCallSummary(const char* when);

}  // namespace rex::diagnostics
