// switch-game: every libnx thread of the process, its stack and its name
// (see nx_threads.h).

#include "nx_threads.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <switch.h>

extern "C" {
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                           int prio, int cpuid);
Result __real_threadClose(Thread* t);
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg);
}

namespace theft4::nx_threads {
namespace {

constexpr int kMaxThreads = 256;
constexpr uint64_t kPattern = 0xA5A5A5A5A5A5A5A5ull;
// Left unpainted under the initial sp: the thread entry's first frame.
constexpr uint64_t kTopMargin = 256;

struct Entry {
  std::atomic<uint32_t> state{0};  // 0 not yet published, 1 live, 2 closed
  uint32_t handle = 0;
  uint64_t id = 0;        // Horizon thread id, 0 until read
  uint64_t lo = 0;        // stack bottom
  uint64_t hi = 0;        // initial sp
  uint64_t paint_hi = 0;  // the pattern covers [lo, paint_hi); 0 = not painted
  uint64_t start = 0;     // pthread start routine, else the libnx entry
  uint64_t deepest = 0;   // bytes below hi in use at the last scan
  char name[40] = {};
};

Entry g_entries[kMaxThreads];
std::atomic<int> g_count{0};
std::atomic<uint32_t> g_dropped{0};
// A scan against threadClose, which unmaps the stack.
std::mutex g_scan_mutex;
bool g_watch = false;
uint64_t g_module_base = 0;
thread_local uint64_t t_pending_start = 0;

Entry* Add(uint32_t handle, uint64_t lo, uint64_t hi, uint64_t start) {
  const int i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxThreads) {
    g_count.store(kMaxThreads, std::memory_order_relaxed);
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  Entry& e = g_entries[i];
  e.handle = handle;
  e.lo = lo;
  e.hi = hi;
  e.start = start;
  return &e;
}

void Publish(Entry& e) { e.state.store(1, std::memory_order_release); }

int Count() { return std::min(g_count.load(std::memory_order_acquire), kMaxThreads); }

uint64_t ThreadId(Entry& e) {
  if (!e.id) {
    u64 id = 0;
    if (R_SUCCEEDED(svcGetThreadId(&id, e.handle))) {
      e.id = id;
    }
  }
  return e.id;
}

// Bytes in use below hi: the first word from the bottom that is not the
// pattern. Called with g_scan_mutex held and the entry live.
uint64_t Deepest(const Entry& e) {
  const auto* p = reinterpret_cast<const uint64_t*>(e.lo);
  const auto* end = reinterpret_cast<const uint64_t*>(e.paint_hi);
  while (p < end && *p == kPattern) {
    ++p;
  }
  return e.hi - reinterpret_cast<uint64_t>(p);
}

// Fault-safe text: a fixed buffer, no allocation, no stdio.
struct Line {
  char buf[256];
  size_t n = 0;
  void Add(const char* s) {
    while (*s && n < sizeof(buf) - 2) buf[n++] = *s++;
  }
  void Hex(uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    Add("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
      if (n < sizeof(buf) - 2) buf[n++] = kHex[(v >> shift) & 0xF];
    }
  }
  void Dec(uint64_t v) {
    char d[24];
    int k = 0;
    do {
      d[k++] = char('0' + v % 10);
      v /= 10;
    } while (v && k < 20);
    while (k && n < sizeof(buf) - 2) buf[n++] = d[--k];
  }
  void Offset(uint64_t v) {
    // Module offset when inside the module (addr2line / where.py).
    if (g_module_base && v >= g_module_base && v - g_module_base < 0x10000000) {
      Add("+");
      Hex(v - g_module_base);
    } else {
      Hex(v);
    }
  }
  const char* End() {
    buf[n++] = '\n';
    buf[n] = 0;
    return buf;
  }
};

}  // namespace

void Start(bool stack_watch, uint64_t module_base) {
  g_watch = stack_watch;
  g_module_base = module_base;
  // The main thread: its stack block from svcQueryMemory, painted below the
  // current sp only if the block is a stack mapping of a sane size.
  const uint64_t sp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
  MemoryInfo mi{};
  u32 pi = 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, sp))) {
    return;
  }
  Entry* e = Add(threadGetCurHandle(), mi.addr, mi.addr + mi.size, 0);
  if (!e) {
    return;
  }
  std::snprintf(e->name, sizeof(e->name), "main (ui)");
  ThreadId(*e);
  const uint64_t paint_hi = (sp - 4096) & ~uint64_t(7);
  if (g_watch && (mi.type & MemState_Type) == MemType_MappedMemory && mi.size <= (64u << 20) &&
      paint_hi > mi.addr) {
    std::memset(reinterpret_cast<void*>(mi.addr), 0xA5, paint_hi - mi.addr);
    e->paint_hi = paint_hi;
  }
  Publish(*e);
}

void SetName(uint32_t handle, const char* name) {
  const int n = Count();
  for (int i = 0; i < n; ++i) {
    Entry& e = g_entries[i];
    if (e.state.load(std::memory_order_acquire) == 1 && e.handle == handle) {
      std::snprintf(e.name, sizeof(e.name), "%s", name);
      ThreadId(e);
    }
  }
}

void LogStacks(LogFn log) {
  struct Row {
    uint64_t id, size, deepest, start;
    bool painted;
    char name[40];
  };
  std::vector<Row> rows;
  const int n = Count();
  rows.reserve(size_t(n));
  uint64_t total = 0;
  for (int i = 0; i < n; ++i) {
    Entry& e = g_entries[i];
    if (e.state.load(std::memory_order_acquire) != 1) {
      continue;
    }
    Row r{};
    r.id = ThreadId(e);
    r.size = e.hi - e.lo;
    r.start = e.start;
    r.painted = e.paint_hi != 0;
    if (r.painted) {
      std::lock_guard<std::mutex> lock(g_scan_mutex);
      // Still mapped and readable over the painted range (a stack freed
      // without threadClose would otherwise fault here).
      MemoryInfo mi{};
      u32 pi = 0;
      if (R_FAILED(svcQueryMemory(&mi, &pi, e.lo)) || !(mi.perm & Perm_R) ||
          mi.addr + mi.size < e.paint_hi) {
        e.state.store(2, std::memory_order_release);
        continue;
      }
      if (e.state.load(std::memory_order_acquire) == 1) {
        e.deepest = Deepest(e);
      }
    }
    r.deepest = e.deepest;
    std::memcpy(r.name, e.name, sizeof(r.name));
    r.name[sizeof(r.name) - 1] = 0;
    total += r.size;
    rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return double(a.deepest) / double(a.size ? a.size : 1) >
           double(b.deepest) / double(b.size ? b.size : 1);
  });
  char line[256];
  for (const Row& r : rows) {
    char start[48] = "";
    if (r.start) {
      if (g_module_base && r.start >= g_module_base) {
        std::snprintf(start, sizeof(start), "; start +0x%" PRIx64, r.start - g_module_base);
      } else {
        std::snprintf(start, sizeof(start), "; start 0x%" PRIx64, r.start);
      }
    }
    const double pct = r.size ? 100.0 * double(r.deepest) / double(r.size) : 0.0;
    if (r.painted) {
      std::snprintf(line, sizeof(line),
                    "stack id %" PRIu64 " '%s': size %" PRIu64 " KiB, deepest use %" PRIu64
                    " KiB (%.1f%%)%s%s",
                    r.id, r.name[0] ? r.name : "?", r.size >> 10, r.deepest >> 10, pct,
                    r.deepest >= r.size ? " FULL (the pattern is gone down to the bottom)"
                    : pct >= 75.0       ? " NEAR FULL"
                                        : "",
                    start);
    } else {
      std::snprintf(line, sizeof(line),
                    "stack id %" PRIu64 " '%s': size %" PRIu64 " KiB, not painted%s", r.id,
                    r.name[0] ? r.name : "?", r.size >> 10, start);
    }
    log(line);
  }
  std::snprintf(line, sizeof(line),
                "stacks: %zu live threads, %" PRIu64 " MiB of stacks, highest share %s; %u "
                "threads beyond the table (%d entries)",
                rows.size(), total >> 20,
                rows.empty() ? "-" : rows.front().name[0] ? rows.front().name : "?",
                g_dropped.load(std::memory_order_relaxed), kMaxThreads);
  log(line);
}

bool FindById(uint64_t id, Info* out) {
  const int n = Count();
  for (int i = 0; i < n; ++i) {
    Entry& e = g_entries[i];
    if (e.state.load(std::memory_order_acquire) != 1 || ThreadId(e) != id) {
      continue;
    }
    out->found = true;
    out->handle = e.handle;
    out->id = e.id;
    out->stack_lo = e.lo;
    out->stack_hi = e.hi;
    out->start = e.start;
    std::memcpy(out->name, e.name, sizeof(out->name));
    out->name[sizeof(out->name) - 1] = 0;
    return true;
  }
  return false;
}

void QueryBlock(uint64_t address, uint64_t* lo, uint64_t* hi) {
  *lo = *hi = 0;
  MemoryInfo mi{};
  u32 pi = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, address)) &&
      (mi.type & MemState_Type) != MemType_Unmapped) {
    *lo = mi.addr;
    *hi = mi.addr + mi.size;
  }
}

void WriteAllThreads(WriteFn write, uint64_t self_id) {
  struct Row {
    uint32_t entry;
    Result pause_rc, context_rc;
    uint64_t pc, lr, sp, fp;
  };
  // Static: the fault handler runs on the exception stack, one fault at a time.
  static Row rows[kMaxThreads];
  const int n = Count();
  int k = 0;
  // Pause, read, resume each thread first; write afterwards, so that no
  // thread stays paused while this one waits on the SD card.
  for (int i = 0; i < n; ++i) {
    Entry& e = g_entries[i];
    if (e.state.load(std::memory_order_acquire) != 1 || ThreadId(e) == self_id) {
      continue;
    }
    Row& r = rows[k++];
    r = Row{};
    r.entry = uint32_t(i);
    r.pause_rc = svcSetThreadActivity(e.handle, ThreadActivity_Paused);
    ThreadContext ctx{};
    r.context_rc = svcGetThreadContext3(&ctx, e.handle);
    if (R_SUCCEEDED(r.context_rc)) {
      r.pc = ctx.pc.x;
      r.lr = ctx.lr;
      r.sp = ctx.sp;
      r.fp = ctx.fp;
    }
    if (R_SUCCEEDED(r.pause_rc)) {
      svcSetThreadActivity(e.handle, ThreadActivity_Runnable);
    }
  }
  for (int j = 0; j < k; ++j) {
    const Row& r = rows[j];
    const Entry& e = g_entries[r.entry];
    Line l;
    l.Add("thread id ");
    l.Dec(e.id);
    l.Add(" '");
    l.Add(e.name[0] ? e.name : "?");
    l.Add("'");
    if (R_FAILED(r.context_rc)) {
      l.Add(": no context, pause rc ");
      l.Hex(r.pause_rc);
      l.Add(", context rc ");
      l.Hex(r.context_rc);
    } else {
      l.Add(": pc ");
      l.Offset(r.pc);
      l.Add(" lr ");
      l.Offset(r.lr);
      l.Add(" sp ");
      l.Hex(r.sp);
      l.Add(r.sp >= e.lo && r.sp <= e.hi ? " (in its stack)" : " (OUTSIDE its stack ");
      if (!(r.sp >= e.lo && r.sp <= e.hi)) {
        l.Hex(e.lo);
        l.Add("-");
        l.Hex(e.hi);
        l.Add(")");
      }
    }
    if (e.start) {
      l.Add(" start ");
      l.Offset(e.start);
    }
    write(l.End());
  }
}

}  // namespace theft4::nx_threads

extern "C" {

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg) {
  theft4::nx_threads::t_pending_start = reinterpret_cast<uint64_t>(start_routine);
  const int rc = __real_pthread_create(thread, attr, start_routine, arg);
  theft4::nx_threads::t_pending_start = 0;
  return rc;
}

// libnx: [stack_mirror, stack_mirror + stack_sz) is the stack and its top the
// initial sp (TLS and reent are mapped above it); the thread has not started
// yet, so the pattern can go in.
Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                           int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_FAILED(rc)) {
    return rc;
  }
  using namespace theft4::nx_threads;
  const uint64_t pending = t_pending_start;
  const uint64_t lo = reinterpret_cast<uint64_t>(t->stack_mirror);
  if (auto* e = Add(t->handle, lo, lo + t->stack_sz,
                    pending ? pending : reinterpret_cast<uint64_t>(entry))) {
    if (g_watch && t->stack_sz > kTopMargin + 4096) {
      e->paint_hi = (e->hi - kTopMargin) & ~uint64_t(7);
      std::memset(reinterpret_cast<void*>(lo), 0xA5, e->paint_hi - lo);
    }
    Publish(*e);
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  {
    using namespace theft4::nx_threads;
    std::lock_guard<std::mutex> lock(g_scan_mutex);
    const int n = Count();
    for (int i = 0; i < n; ++i) {
      Entry& e = g_entries[i];
      if (e.handle == t->handle && e.state.load(std::memory_order_acquire) == 1) {
        e.state.store(2, std::memory_order_release);
      }
    }
  }
  return __real_threadClose(t);
}

}  // extern "C"
