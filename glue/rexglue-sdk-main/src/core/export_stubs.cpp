/**
 * @file core/export_stubs.cpp
 * @brief First-call log, counts and the optional stop for stub exports; on
 *        Switch, the trace of the implemented exports (per export, per thread).
 */

#include <rex/diagnostics/export_stubs.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/diagnostics/missing_function.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if REX_PLATFORM_NX
extern "C" {
#include <switch/arm/counter.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
}
#endif

REXCVAR_DEFINE_BOOL(nx_fatal_stubs, false, "Kernel",
                    "Stop the run at the first Xbox export that is only a stub (REX_EXPORT_STUB) "
                    "instead of logging it and going on");

namespace rex::diagnostics {
namespace {

std::mutex g_stub_list_mutex;
ExportStubSite* g_stub_list = nullptr;

const char* ExportName(const char* symbol) {
  constexpr const char kPrefix[] = "__imp__";
  return std::strncmp(symbol, kPrefix, sizeof(kPrefix) - 1) == 0 ? symbol + sizeof(kPrefix) - 1
                                                                 : symbol;
}

}  // namespace

void RecordExportStubHit(ExportStubSite& site, uint32_t guest_lr) {
  if (site.hits.fetch_add(1, std::memory_order_relaxed) == 0) {
    {
      std::lock_guard<std::mutex> lock(g_stub_list_mutex);
      site.next = g_stub_list;
      g_stub_list = &site;
    }
    REXKRNL_WARN("export stub, first call: {} (guest LR {:08X})", ExportName(site.name),
                 guest_lr);
  }
  if (REXCVAR_GET(nx_fatal_stubs)) {
    StopAtMissingFunction("Xbox export is only a stub (nx_fatal_stubs)", ExportName(site.name),
                          guest_lr);
  }
}

void LogExportStubSummary(const char* when) {
  std::vector<std::pair<const char*, uint64_t>> hits;
  {
    std::lock_guard<std::mutex> lock(g_stub_list_mutex);
    for (ExportStubSite* site = g_stub_list; site; site = site->next) {
      hits.emplace_back(ExportName(site->name), site->hits.load(std::memory_order_relaxed));
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  uint64_t total = 0;
  for (const auto& [name, count] : hits) {
    total += count;
  }
  REXKRNL_INFO("export stubs {}: {} distinct, {} calls", when, hits.size(), total);
  for (const auto& [name, count] : hits) {
    REXKRNL_INFO("  {:>10}  {}", count, name);
  }
}

// ── Implemented exports ────────────────────────────────────────────────────

namespace {

std::mutex g_export_list_mutex;
ExportCallSite* g_export_list = nullptr;
ExportThreadTrace* g_export_threads = nullptr;
thread_local ExportThreadTrace* t_export_trace = nullptr;

uint64_t MonotonicTicks() {
#if REX_PLATFORM_NX
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double TicksToSeconds(uint64_t ticks) {
#if REX_PLATFORM_NX
  return double(armTicksToNs(ticks)) / 1e9;
#else
  return std::chrono::duration<double>(std::chrono::steady_clock::duration(ticks)).count();
#endif
}

ExportThreadTrace* ThisThreadTrace() {
  if (!t_export_trace) {
    auto* trace = new ExportThreadTrace();
#if REX_PLATFORM_NX
    trace->nx_handle = threadGetCurHandle();
    svcGetThreadId(&trace->nx_thread_id, CUR_THREAD_HANDLE);
#else
    trace->nx_thread_id = rex::thread::current_thread_id();
#endif
    std::lock_guard<std::mutex> lock(g_export_list_mutex);
    trace->next = g_export_threads;
    g_export_threads = trace;
    t_export_trace = trace;
  }
  return t_export_trace;
}

// Single writer: load and store, not fetch_add (A57 has no LSE atomics).
template <typename T>
void BumpOwn(std::atomic<T>& value) {
  value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

}  // namespace

static std::atomic<ExportEntryHook> g_export_entry_hook{nullptr};

// Same LR -> its slot; else an empty slot; else the slot picked by the LR.
static void SampleCaller(ExportCallSite& site, uint32_t guest_lr) {
  int slot = -1;
  for (int i = 0; i < 8 && slot < 0; ++i) {
    const uint32_t lr = site.sample_lr[i].load(std::memory_order_relaxed);
    if (lr == guest_lr || lr == 0) slot = i;
  }
  if (slot < 0) {
    slot = int((guest_lr >> 2) & 7);
    site.sample_hits[slot].store(0, std::memory_order_relaxed);
  }
  site.sample_lr[slot].store(guest_lr, std::memory_order_relaxed);
  site.sample_hits[slot].store(site.sample_hits[slot].load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
}

void SetExportEntryHook(ExportEntryHook hook) {
  g_export_entry_hook.store(hook, std::memory_order_release);
}

void RunExportEntryHook() {
  if (ExportEntryHook hook = g_export_entry_hook.load(std::memory_order_acquire)) {
    hook();
  }
}

bool g_export_tracing = true;

void SetExportTracing(bool enabled) { g_export_tracing = enabled; }

double MeasureExportTraceNs(uint32_t iterations) {
  static ExportCallSite site{"__imp__ExportTraceBenchmark"};
  site.calls.store(1, std::memory_order_relaxed);  // not a first call: never listed
  ExportThreadTrace* trace = ThisThreadTrace();
  const char* last = trace->last.load(std::memory_order_relaxed);
  const uint32_t lr = trace->guest_lr.load(std::memory_order_relaxed);
  const uint64_t calls = trace->calls.load(std::memory_order_relaxed);
  const uint64_t start = MonotonicTicks();
  for (uint32_t i = 0; i < iterations; ++i) {
    bool first = false;
    const char* previous = ExportEnter(site, 0x82000000u + i, &first);
    ExportLeave(previous, nullptr, 0);
  }
  const double seconds = TicksToSeconds(MonotonicTicks() - start);
  trace->last.store(last, std::memory_order_relaxed);
  trace->guest_lr.store(lr, std::memory_order_relaxed);
  trace->calls.store(calls, std::memory_order_relaxed);
  return iterations ? seconds * 1e9 / double(iterations) : 0.0;
}

const char* ExportEnter(ExportCallSite& site, uint32_t guest_lr, bool* first) {
  if (ExportEntryHook hook = g_export_entry_hook.load(std::memory_order_acquire)) {
    hook();
  }
  ExportThreadTrace* trace = ThisThreadTrace();
  if (site.calls.fetch_add(1, std::memory_order_relaxed) == 0) {
    *first = true;
    {
      std::lock_guard<std::mutex> lock(g_export_list_mutex);
      site.next = g_export_list;
      g_export_list = &site;
    }
    REXKRNL_DEBUG("export first call: {} (guest LR {:08X}, thread {:x})", ExportName(site.name),
                  guest_lr, trace->nx_thread_id);
  }
  const char* previous = trace->current.load(std::memory_order_relaxed);
  trace->last.store(site.name, std::memory_order_relaxed);
  trace->guest_lr.store(guest_lr, std::memory_order_relaxed);
  trace->enter_ticks.store(MonotonicTicks(), std::memory_order_relaxed);
  BumpOwn(trace->calls);
  if ((trace->calls.load(std::memory_order_relaxed) & 255) == 0) {
    SampleCaller(site, guest_lr);
  }
  trace->current.store(site.name, std::memory_order_release);
  return previous;
}

void ExportLeave(const char* previous, ExportCallSite* first_site, uint64_t r3) {
  ExportThreadTrace* trace = t_export_trace;
  if (trace) {
    trace->current.store(previous, std::memory_order_release);
  }
  if (first_site) {
    REXKRNL_DEBUG("export first result: {} -> {:08X} (thread {:x})", ExportName(first_site->name),
                  uint32_t(r3), trace ? trace->nx_thread_id : 0);
  }
}

size_t SnapshotExportThreads(ExportThreadSnapshot* out, size_t capacity) {
  const uint64_t now = MonotonicTicks();
  std::lock_guard<std::mutex> lock(g_export_list_mutex);
  size_t n = 0;
  for (ExportThreadTrace* t = g_export_threads; t && n < capacity; t = t->next, ++n) {
    ExportThreadSnapshot& s = out[n];
    const char* current = t->current.load(std::memory_order_acquire);
    const char* last = t->last.load(std::memory_order_relaxed);
    const uint64_t entered = t->enter_ticks.load(std::memory_order_relaxed);
    s.current = current ? ExportName(current) : nullptr;
    s.last = last ? ExportName(last) : nullptr;
    s.seconds_in_current = current && now > entered ? TicksToSeconds(now - entered) : 0;
    s.guest_lr = t->guest_lr.load(std::memory_order_relaxed);
    s.calls = t->calls.load(std::memory_order_relaxed);
    s.nx_handle = t->nx_handle;
    s.nx_thread_id = t->nx_thread_id;
  }
  return n;
}

void GetExportCallTotals(size_t* distinct, uint64_t* calls) {
  size_t d = 0;
  uint64_t c = 0;
  {
    std::lock_guard<std::mutex> lock(g_export_list_mutex);
    for (ExportCallSite* site = g_export_list; site; site = site->next) {
      ++d;
      c += site->calls.load(std::memory_order_relaxed);
    }
  }
  if (distinct) *distinct = d;
  if (calls) *calls = c;
}

void LogExportCallSummary(const char* when) {
  std::vector<std::pair<const char*, uint64_t>> calls;
  {
    std::lock_guard<std::mutex> lock(g_export_list_mutex);
    for (ExportCallSite* site = g_export_list; site; site = site->next) {
      calls.emplace_back(ExportName(site->name), site->calls.load(std::memory_order_relaxed));
    }
  }
  std::sort(calls.begin(), calls.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  uint64_t total = 0;
  for (const auto& [name, count] : calls) {
    total += count;
  }
  REXKRNL_INFO("exports called {}: {} distinct, {} calls", when, calls.size(), total);
  for (const auto& [name, count] : calls) {
    REXKRNL_INFO("  {:>10}  {}", count, name);
  }
}

void LogExportCallRates(const char* when, double seconds, size_t top) {
  struct Row {
    ExportCallSite* site;
    uint64_t delta;
  };
  std::vector<Row> rows;
  {
    std::lock_guard<std::mutex> lock(g_export_list_mutex);
    for (ExportCallSite* site = g_export_list; site; site = site->next) {
      const uint64_t now = site->calls.load(std::memory_order_relaxed);
      rows.push_back({site, now - site->calls_at_last_rates});
      site->calls_at_last_rates = now;
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.delta > b.delta; });
  uint64_t total = 0;
  for (const Row& r : rows) total += r.delta;
  REXKRNL_INFO("export rates {} (last {:.0f} s): {:.0f} calls/s in all", when, seconds,
               seconds > 0 ? double(total) / seconds : 0.0);
  for (size_t i = 0; i < rows.size() && i < top; ++i) {
    ExportCallSite* site = rows[i].site;
    uint32_t sum = 0;
    for (int k = 0; k < 8; ++k) sum += site->sample_hits[k].load(std::memory_order_relaxed);
    std::string callers;
    for (int k = 0; k < 8; ++k) {
      const uint32_t lr = site->sample_lr[k].load(std::memory_order_relaxed);
      const uint32_t hits = site->sample_hits[k].load(std::memory_order_relaxed);
      if (lr && hits && sum) {
        callers += fmt::format(" {:08X}:{}%", lr, hits * 100 / sum);
      }
    }
    REXKRNL_INFO("  {:>10.0f}/s  {}  callers{}", seconds > 0 ? double(rows[i].delta) / seconds : 0.0,
                 ExportName(site->name), callers.empty() ? " (no sample)" : callers);
  }
}

}  // namespace rex::diagnostics
