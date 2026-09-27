// switch-smoke: on-device smoke test for the ReXGlue SDK on Nintendo Switch.
//
// Steps, in order; each prints its outcome on screen and in
// sdmc:/switch/theft4/smoke.log (through the SDK logger):
//   1. SDK logging
//   2. svcGetInfo address-space layout (alias, heap, ASLR regions)
//   3. guest memory through the runtime itself: rex::memory::Memory::Initialize
//      (design A, docs/switch-port/03-memory.md), translation of every guest
//      window, commit on demand, aliases 0x80/0x90 and the physical views,
//      release
//   4. four threads through rex::thread::Thread, one per core of the process
//      core mask; each must run on its core, and logical_processor_count()
//      must match the mask; then the guest CPU -> core mapping of
//      XThread::SetActiveCpu (soft and hard affinity, core 3 left to host
//      workers), and with core 3 (mask 0xF) one host worker placed on it;
//      then the Horizon priority bands (guest 0x3B, host 0x2B-0x2D) and a
//      measurement that only 0x3B is time-sliced
// Then "SMOKE OK" or "SMOKE FAIL at <step>: <reason>" for those four steps,
// followed by the exploratory probes T1-T10 in probes.cpp (alias primitives,
// heap size, cost of the guest-memory design A). Press + to exit.
//
// Every smoke line is made durable before the next operation: it is appended
// to smoke.log through its own fd, fsync'd and closed. A crash therefore
// leaves the last attempted operation as the last line of the log. The SDK
// logger writes through the same path (DurableSink), in order with the smoke
// lines: Horizon's FS refuses to open a file for writing while another
// handle has it open for writing, so a second writer on smoke.log loses
// every line (run 5 kept only the SDK's three).

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/flags.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include <spdlog/sinks/base_sink.h>

#include "probes.h"

namespace {

constexpr const char* kLogDir = "sdmc:/switch/theft4";
constexpr const char* kLogPath = "sdmc:/switch/theft4/smoke.log";
// Optional cvars, e.g. "nx_memory_trace = true"; argv (nxlink) works too.
constexpr const char* kConfigPath = "sdmc:/switch/theft4/smoke.toml";

std::string g_first_failure;
bool g_logging_ready = false;

}  // namespace

// Start of the NRO image: libnx crt0's entry label, first thing in .text at
// ELF address 0, so its runtime address is the module base. Not switch.ld's
// __start__: that one is an absolute symbol (PROVIDE_HIDDEN(__start__ = 0x0)),
// which a PIE link does not relocate, so &__start__ is 0 at run time.
extern "C" char _start;
int main(int argc, char** argv);

REXCVAR_DECLARE(bool, nx_memory_trace);

namespace {

// Appends to smoke.log and forces it to storage: open, write, fsync, close.
// One writer at a time (see the file header). The first failed open is
// reported on screen instead of being lost.
std::mutex g_log_file_mutex;

void DurableWrite(const char* data, size_t size) {
  std::lock_guard<std::mutex> lock(g_log_file_mutex);
  int fd = open(kLogPath, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd < 0) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      std::printf("smoke.log: open failed (errno %d); lines are on screen only\n", errno);
      consoleUpdate(nullptr);
    }
    return;
  }
  (void)write(fd, data, size);
  fsync(fd);
  close(fd);
}

void DurableAppend(const char* line) {
  std::string text(line);
  text += '\n';
  DurableWrite(text.data(), text.size());
}

// The SDK logger's only sink: each formatted line goes through DurableWrite.
class DurableSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    DurableWrite(formatted.data(), formatted.size());
  }
  void flush_() override {}
};

}  // namespace

void Report(bool error, const char* fmt, ...) {
  const char* prefix = error ? "[smoke] ERROR " : "[smoke] ";
  char line[512];
  const size_t prefix_len = std::strlen(prefix);
  std::memcpy(line, prefix, prefix_len);
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line + prefix_len, sizeof(line) - prefix_len, fmt, args);
  va_end(args);
  std::printf("%s\n", line + std::strlen("[smoke] "));
  consoleUpdate(nullptr);
  if (g_logging_ready) {
    DurableAppend(line);
  }
}

namespace {

void Fail(const char* step, const std::string& reason) {
  Report(true, "FAIL [%s] %s", step, reason.c_str());
  if (g_first_failure.empty()) {
    g_first_failure = std::string(step) + ": " + reason;
  }
}

std::string Hex(uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%" PRIx64, value);
  return buf;
}

// ── 1. Logging ──────────────────────────────────────────────────────────────

void StepLogging() {
  mkdir("sdmc:/switch", 0777);
  mkdir(kLogDir, 0777);
  // Without a diagnostics policy InitLogging and every REXSYS_* line are
  // no-ops (the run-4 log had none of the SDK's own errors).
  std::string policy_error;
  const bool policy_ok = rex::diagnostics::Configure(true, "logging", &policy_error);
  // No file sink of its own: see the file header.
  auto sink = std::make_shared<DurableSink>();
  sink->set_pattern("[sdk] [%l] [%n] [t%t] %v");
  rex::LogConfig config;
  config.default_level = spdlog::level::debug;
  config.extra_sinks.push_back(sink);
  g_logging_ready = true;
  rex::InitLogging(config);
  REXLOG_INFO("[smoke] SDK logger initialised");
  SMOKE_INFO("step 1 logging: SDK logger and smoke lines both appended to %s", kLogPath);
  rex::cvar::LoadConfig(kConfigPath);
  rex::cvar::FinalizeInit();
  SMOKE_INFO("  cvars (argv, then %s): nx_memory_trace %s", kConfigPath,
             REXCVAR_GET(nx_memory_trace) ? "on" : "off");
  if (!policy_ok) {
    Fail("logging", "diagnostics::Configure(logging): " + policy_error);
  }
}

// Runtime addresses to match crash-report PCs against the ELF:
// ELF address = runtime address - module base.
void StepModuleBase() {
  const u64 module_base = reinterpret_cast<u64>(&_start);
  const u64 main_address = reinterpret_cast<u64>(&main);
  MemoryInfo info{};
  u32 page_info = 0;
  const Result rc = svcQueryMemory(&info, &page_info, main_address);
  SMOKE_INFO("module base (_start) 0x%" PRIx64 ", main 0x%" PRIx64 " = base + 0x%" PRIx64,
             module_base, main_address, main_address - module_base);
  SMOKE_INFO("  code block of main: 0x%" PRIx64 "+0x%" PRIx64 " type 0x%x perm %u (rc 0x%x)",
             info.addr, info.size, info.type, info.perm, rc);
  // The loaded image starts with crt0 (b startup; .word __nx_mod0 - _start)
  // and the NRO header, magic "NRO0" at +0x10; the text segment is the
  // R-X code block that starts at the base.
  uint32_t nro_magic = 0;
  std::memcpy(&nro_magic, reinterpret_cast<const void*>(module_base + 0x10), sizeof(nro_magic));
  const bool ok = R_SUCCEEDED(rc) && info.addr == module_base && info.perm == Perm_Rx &&
                  module_base % 0x1000 == 0 && nro_magic == 0x304F524Eu;  // "NRO0"
  SMOKE_INFO("  base check: code block starts at the base, R-X, page aligned, \"NRO0\" at +0x10 "
             "(%08X) -> %s",
             nro_magic, ok ? "ok" : "MISMATCH");
  if (!ok) {
    Fail("module base", "_start " + Hex(module_base) + " is not the start of the NRO image");
  }
}

// ── 2. Address space ────────────────────────────────────────────────────────

void StepAddressSpace() {
  struct Item {
    const char* name;
    u32 id;
  };
  static const Item kItems[] = {
      {"alias region start", InfoType_AliasRegionAddress},
      {"alias region size", InfoType_AliasRegionSize},
      {"heap region start", InfoType_HeapRegionAddress},
      {"heap region size", InfoType_HeapRegionSize},
      {"ASLR region start", InfoType_AslrRegionAddress},
      {"ASLR region size", InfoType_AslrRegionSize},
      {"total memory", InfoType_TotalMemorySize},
      {"used memory", InfoType_UsedMemorySize},
      {"core mask", InfoType_CoreMask},
  };
  SMOKE_INFO("step 2 svcGetInfo:");
  for (const Item& item : kItems) {
    u64 value = 0;
    Result rc = svcGetInfo(&value, item.id, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc)) {
      Fail("svcGetInfo", std::string(item.name) + " rc=" + Hex(rc));
      continue;
    }
    SMOKE_INFO("  %-20s %s", item.name, Hex(value).c_str());
  }
  // 0 = application (e.g. a forwarder or title takeover), 2 = library applet
  // (album): the core mask and the memory above depend on it.
  SMOKE_INFO("  %-20s %d", "applet type", int(appletGetAppletType()));

  // Memory SVCs a Horizon guest-memory design could rely on
  // (docs/switch-port/03-memory.md). "hinted" means the loader reports the
  // syscall as available to this process.
  struct Svc {
    const char* name;
    unsigned id;
  };
  static const Svc kSvcs[] = {
      {"svcMapMemory", 0x04},          {"svcMapSharedMemory", 0x13},
      {"svcCreateTransferMemory", 0x15}, {"svcMapPhysicalMemory", 0x2C},
      {"svcCreateCodeMemory", 0x4B},   {"svcControlCodeMemory", 0x4C},
      {"svcCreateSharedMemory", 0x50}, {"svcMapTransferMemory", 0x51},
  };
  SMOKE_INFO("  syscall hints:");
  for (const Svc& svc : kSvcs) {
    SMOKE_INFO("    %-24s (0x%02x) %s", svc.name, svc.id,
               envIsSyscallHinted(svc.id) ? "hinted" : "NOT hinted");
  }
}

// ── 3. Guest memory ─────────────────────────────────────────────────────────

// The runtime's own guest memory: rex::memory::Memory::Initialize()
// (src/system/xmemory.cpp, NX branch) on top of memory_switch.cpp (design A,
// docs/switch-port/03-memory.md). Kept alive for T9, destroyed at exit.
std::unique_ptr<rex::memory::Memory> g_memory;

rex::memory::nx::GuestArenaStats LogArenaStats(const char* when) {
  const auto stats = rex::memory::nx::GetGuestArenaStats();
  SMOKE_INFO("  arena %s: %zu blocks mapped (%zu MiB, peak %zu), %zu pages committed "
             "(%zu references), %" PRIu64 " maps / %" PRIu64 " unmaps, %.2f ms in the map SVCs",
             when, stats.mapped_blocks, stats.mapped_blocks * 2, stats.peak_mapped_blocks,
             stats.committed_pages, stats.page_references, stats.map_calls, stats.unmap_calls,
             double(armTicksToNs(stats.map_ticks)) / 1e6);
  return stats;
}

// memory_switch.cpp trace (every SVC of the commit/protect path with its
// Result, and the failing step) into the smoke log, for the duration of a
// scope, when the cvar nx_memory_trace is on (off by default: each line is a
// durable append to the SD card). Not used around faults: the sink would run
// in the exception handler.
void TraceLine(const char* line) {
  SMOKE_INFO("    trace %s", line);
}

struct ScopedMemoryTrace {
  ScopedMemoryTrace() {
    if (REXCVAR_GET(nx_memory_trace)) {
      rex::memory::nx::SetMemoryTraceSink(TraceLine);
    }
  }
  ~ScopedMemoryTrace() { rex::memory::nx::SetMemoryTraceSink(nullptr); }
};

// The heap's own view of a page Initialize committed: a failed host commit
// leaves the page table without the commit state.
bool CheckCommitted(rex::memory::Memory& memory, uint32_t guest, const char* what) {
  uint32_t protect = 0;
  const bool ok = memory.LookupHeap(guest)->QueryProtect(guest, &protect) &&
                  (protect & rex::memory::kMemoryProtectWrite);
  SMOKE_INFO("  %08X (%s) committed by Initialize: protect %X -> %s", guest, what, protect,
             ok ? "ok" : "NOT COMMITTED");
  if (!ok) {
    Fail("guest memory", Hex(guest) + " (" + what + ") not committed by Memory::Initialize");
  }
  return ok;
}

// Every guest window must land where rex_guest_table says, as seen by the
// runtime's own TranslateVirtual / TranslatePhysical.
bool CheckTranslation(rex::memory::Memory& memory) {
  uint8_t* const v = memory.virtual_membase();
  uint8_t* const p = memory.physical_membase();
  struct Expect {
    uint32_t guest;
    uint8_t* host;
    const char* what;
  };
  const Expect kExpect[] = {
      {0x00001000u, v + 0x1000, "virtual 4K"},
      {0x7EFFFFFCu, v + 0x7EFFFFFCull, "virtual 64K end"},
      {0x7F000010u, p + 0x10, "0x7F writeback -> physical"},
      {0x80000020u, v + 0x80000020ull, "XEX 64K"},
      {0x90000020u, v + 0x80000020ull, "XEX 4K -> 0x80"},
      {0xA0000030u, p + 0x30, "physical 0xA"},
      {0xC0000040u, p + 0x40, "physical 0xC"},
      {0xE0000050u, p + 0x1050, "physical 0xE (+4 KiB)"},
      {0xFFFFEFFCu, p + 0x1FFFFFFC, "physical 0xE end"},
  };
  bool ok = true;
  for (const Expect& e : kExpect) {
    uint8_t* host = memory.TranslateVirtual(e.guest);
    if (host != e.host) {
      Fail("guest memory", "TranslateVirtual(" + Hex(e.guest) + ") = " +
                               Hex(reinterpret_cast<uint64_t>(host)) + ", expected " +
                               Hex(reinterpret_cast<uint64_t>(e.host)) + " (" + e.what + ")");
      ok = false;
    }
  }
  SMOKE_INFO("  translation of 9 guest addresses (all windows): %s", ok ? "ok" : "MISMATCH");
  return ok;
}

bool WriteRead32(const char* label, void* write_host, void* read_host, uint32_t pattern) {
  *static_cast<volatile uint32_t*>(write_host) = pattern;
  const uint32_t read = *static_cast<volatile uint32_t*>(read_host);
  SMOKE_INFO("  %s: wrote %08X at %p, read %08X at %p -> %s", label, pattern, write_host, read,
             read_host, read == pattern ? "ok" : "MISMATCH");
  return read == pattern;
}

void StepGuestMemory() {
  SMOKE_INFO("step 3 guest memory: rex::memory::Memory::Initialize (runtime path, design A)");
  ScopedMemoryTrace trace;
  g_memory = std::make_unique<rex::memory::Memory>();
  SMOKE_INFO("  about to Memory::Initialize");
  if (!g_memory->Initialize()) {
    Fail("guest memory", "Memory::Initialize failed (see log for memory_switch reason)");
    g_memory.reset();
    return;
  }
  rex::memory::Memory& memory = *g_memory;
  SMOKE_INFO("  virtual_membase %p, physical_membase %p",
             static_cast<void*>(memory.virtual_membase()),
             static_cast<void*>(memory.physical_membase()));
  const auto initial = LogArenaStats("after Initialize");
  if (!CheckTranslation(memory)) {
    return;
  }

  bool ok = true;
  // Memory::Initialize ignores the result of its own allocations.
  ok &= CheckCommitted(memory, 0x80000000u, "XEX header page");
  ok &= CheckCommitted(memory, 0xC0000000u, "GPU writeback 16 MiB");
  // XEX alias: Initialize writes 0x2a6e3f38 (big-endian) at 0x8000001C.
  const uint32_t xex80 = *memory.TranslateVirtual<volatile uint32_t*>(0x8000001C);
  const uint32_t xex90 = *memory.TranslateVirtual<volatile uint32_t*>(0x9000001C);
  SMOKE_INFO("  0x8000001C = %08X, 0x9000001C = %08X (expected 383F6E2A both) -> %s", xex80,
             xex90, xex80 == 0x383F6E2Au && xex90 == xex80 ? "ok" : "MISMATCH");
  ok &= xex80 == 0x383F6E2Au && xex90 == xex80;

  // Virtual heap: commit on demand, write, read back, release.
  auto* v40 = memory.LookupHeap(0x40000000);
  uint32_t virtual_address = 0;
  SMOKE_INFO("  about to allocate 4 MiB in the 0x40000000 heap");
  if (!v40->Alloc(4u << 20, 0x10000,
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                  &virtual_address)) {
    Fail("guest memory", "0x40000000 heap Alloc(4 MiB) failed");
    return;
  }
  uint8_t* vh = memory.TranslateVirtual(virtual_address);
  ok &= WriteRead32("virtual first", vh, vh, 0x11223344u);
  ok &= WriteRead32("virtual last ", vh + (4u << 20) - 4, vh + (4u << 20) - 4, 0x55667788u);
  LogArenaStats("with 4 MiB virtual");

  // Physical memory through its four views: allocate in the 0xE0000000 heap,
  // write through 0xE, read through physical_membase, 0xA and 0xC.
  auto* vE = memory.LookupHeapByType(true, 4096);
  uint32_t e_address = 0;
  SMOKE_INFO("  about to allocate 64 KiB in the 0xE0000000 heap");
  if (!vE->Alloc(0x10000, 0x1000,
                 rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                 &e_address)) {
    Fail("guest memory", "0xE0000000 heap Alloc(64 KiB) failed");
    return;
  }
  const uint32_t physical = memory.GetPhysicalAddress(e_address);
  SMOKE_INFO("  0xE heap: guest %08X = physical %08X", e_address, physical);
  uint8_t* eh = memory.TranslateVirtual(e_address);
  ok &= WriteRead32("0xE -> physical", eh, memory.TranslatePhysical(physical), 0xA1B2C3D4u);
  ok &= WriteRead32("0xE -> 0xA     ", eh + 8, memory.TranslateVirtual(0xA0000000u + physical + 8),
                    0x0BADF00Du);
  ok &= WriteRead32("0xC -> 0xE     ", memory.TranslateVirtual(0xC0000000u + physical + 16),
                    eh + 16, 0xCAFEBABEu);
  LogArenaStats("with 64 KiB physical");

  SMOKE_INFO("  about to release both allocations");
  v40->Release(virtual_address);
  vE->Release(e_address);
  const auto released = LogArenaStats("after release");
  if (released.committed_pages != initial.committed_pages ||
      released.page_references != initial.page_references) {
    Fail("guest memory", "release left " + std::to_string(released.committed_pages) +
                             " pages / " + std::to_string(released.page_references) +
                             " references, expected the post-Initialize " +
                             std::to_string(initial.committed_pages) + " / " +
                             std::to_string(initial.page_references));
    return;
  }
  if (!ok) {
    Fail("guest memory", "write/read-back mismatch (see lines above)");
    return;
  }
  SMOKE_INFO("  guest memory ok");
}

void StepGuestMemoryShutdown() {
  if (!g_memory) {
    return;
  }
  SMOKE_INFO("about to destroy rex::memory::Memory");
  g_memory.reset();
  // Every heap drops its references in ~Memory before the arena goes: nothing
  // may be left committed, referenced or mapped.
  const auto stats = LogArenaStats("after ~Memory");
  if (stats.committed_pages || stats.page_references || stats.mapped_blocks ||
      stats.blocks_mapped_at_release) {
    Fail("guest memory shutdown",
         std::to_string(stats.committed_pages) + " pages committed, " +
             std::to_string(stats.page_references) + " references, " +
             std::to_string(stats.mapped_blocks) + " blocks mapped, " +
             std::to_string(stats.blocks_mapped_at_release) +
             " blocks still referenced at release (all must be 0)");
  } else {
    SMOKE_INFO("  after ~Memory: 0 pages committed, 0 references, 0 blocks mapped -> ok");
  }
}

// ── 4. Threads ──────────────────────────────────────────────────────────────

// Horizon priority bands (04-threads-exit.md section 7): the policy table,
// the explicit default of a runtime thread, set_nx_priority read back, and
// the premise itself: two threads spinning on one core start together at 0x3B
// (time-sliced) and one after the other at 0x2C (cooperative).
int64_t NxSecondStartDelayMs(int32_t priority, uint32_t core) {
  constexpr uint64_t kSpinMs = 200;
  std::atomic<uint64_t> start_ticks[2] = {};
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  for (int n = 0; n < 2; ++n) {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 64 * 1024;
    params.create_suspended = true;
    auto thread = rex::thread::Thread::Create(params, [n, &start_ticks] {
      const uint64_t t0 = armGetSystemTick();
      start_ticks[n].store(t0);
      const uint64_t spin = armNsToTicks(kSpinMs * 1000000ull);
      while (armGetSystemTick() - t0 < spin) {
      }
    });
    if (!thread) {
      return -1;
    }
    thread->set_nx_priority(priority);
    thread->set_ideal_core(core, uint64_t(1) << core);
    threads.push_back(std::move(thread));
  }
  for (auto& thread : threads) {
    thread->Resume();
  }
  for (auto& thread : threads) {
    if (rex::thread::Wait(thread.get(), false, std::chrono::milliseconds(5000)) !=
        rex::thread::WaitResult::kSuccess) {
      return -1;
    }
  }
  const uint64_t a = start_ticks[0].load(), b = start_ticks[1].load();
  return int64_t(armTicksToNs(a > b ? a - b : b - a) / 1000000ull);
}

void StepThreadPriorities(uint64_t core_mask) {
  struct Expect {
    bool guest;
    const char* name;
    int32_t priority;
  };
  const Expect kExpect[] = {
      {true, "XThread0004 (F8000010)", rex::thread::kNxPriorityGuest},
      {false, "Audio Worker (F8000014)", rex::thread::kNxPriorityAudio},
      {false, "XMA Decoder (F8000018)", rex::thread::kNxPriorityAudio},
      {false, "GPU Commands (F800001C)", rex::thread::kNxPriorityGpu},
      {false, "GPU VSync (F8000020)", rex::thread::kNxPriorityHost},
      {false, "Kernel Host Tasks (F8000004)", rex::thread::kNxPriorityHost},
      {false, "Vulkan Pipelines", rex::thread::kNxPriorityGuest},
      {false, "some host thread", rex::thread::kNxPriorityHost},
  };
  bool table_ok = true;
  for (const Expect& e : kExpect) {
    const int32_t got = rex::thread::nx_priority_for_thread(e.guest, e.name);
    if (got != e.priority) {
      table_ok = false;
      Fail("threads", std::string("priority for '") + e.name + "' " + Hex(uint64_t(got)) +
                          ", expected " + Hex(uint64_t(e.priority)));
    }
  }
  SMOKE_INFO("  priority bands: guest 0x3B, audio 0x2B, host 0x2C, GPU commands 0x2D, "
             "pipelines 0x3B -> %s", table_ok ? "ok" : "MISMATCH");

  std::atomic<int32_t> default_priority{-1};
  rex::thread::Thread::CreationParameters params;
  params.stack_size = 64 * 1024;
  params.create_suspended = true;
  auto thread = rex::thread::Thread::Create(params, [&default_priority] {
    s32 prio = -1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    default_priority.store(prio);
  });
  if (!thread) {
    Fail("threads", "Thread::Create failed for the priority check");
    return;
  }
  thread->Resume();
  rex::thread::Wait(thread.get(), false, std::chrono::milliseconds(5000));
  const bool default_ok = default_priority.load() == rex::thread::kNxPriorityHost;
  SMOKE_INFO("  runtime thread default priority %s (expected 0x2C) -> %s",
             Hex(uint64_t(default_priority.load())).c_str(), default_ok ? "ok" : "WRONG");
  if (!default_ok) {
    Fail("threads", "runtime thread default priority " + Hex(uint64_t(default_priority.load())));
  }

  params.create_suspended = true;
  std::atomic<int32_t> seen{-1};
  auto guest_like = rex::thread::Thread::Create(params, [&seen] {
    s32 prio = -1;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    seen.store(prio);
  });
  if (guest_like) {
    guest_like->set_nx_priority(rex::thread::kNxPriorityGuest);
    const int32_t read_back = guest_like->priority();
    guest_like->Resume();
    rex::thread::Wait(guest_like.get(), false, std::chrono::milliseconds(5000));
    const bool ok = read_back == rex::thread::kNxPriorityGuest &&
                    seen.load() == rex::thread::kNxPriorityGuest;
    SMOKE_INFO("  set_nx_priority(0x3B): priority() %s, seen by the thread %s -> %s",
               Hex(uint64_t(read_back)).c_str(), Hex(uint64_t(seen.load())).c_str(),
               ok ? "ok" : "WRONG");
    if (!ok) {
      Fail("threads", "set_nx_priority(0x3B) read back " + Hex(uint64_t(read_back)));
    }
  }

  // Time slicing, measured: the core is the highest guest core, away from
  // the smoke's own thread where possible.
  const uint64_t guest_mask = core_mask & 0x7;
  const uint32_t core = guest_mask ? 63 - uint32_t(__builtin_clzll(guest_mask)) : 0;
  SMOKE_INFO("  about to spin two threads on core %u at 0x3B, then at 0x2C (200 ms each)",
             core);
  const int64_t sliced = NxSecondStartDelayMs(rex::thread::kNxPriorityGuest, core);
  const int64_t cooperative = NxSecondStartDelayMs(rex::thread::kNxPriorityHost, core);
  const bool slicing_ok = sliced >= 0 && sliced < 50 && cooperative >= 150;
  SMOKE_INFO("  second spinner started %lld ms after the first at 0x3B, %lld ms at 0x2C -> %s",
             (long long)sliced, (long long)cooperative,
             slicing_ok ? "ok (only 0x3B is time-sliced)" : "UNEXPECTED");
  if (!slicing_ok) {
    Fail("threads", "time slicing: second start " + std::to_string(sliced) + " ms at 0x3B, " +
                        std::to_string(cooperative) + " ms at 0x2C");
  }
}

// The Switch core split XThread::SetActiveCpu applies (04-threads-exit.md
// section 3): Xbox 360 CPUs 0,1 -> core 0, 2,3 -> 1, 4,5 -> 2, core 3 kept
// for host workers; soft affinity (ideal core + every guest core) unless
// nx_hard_thread_affinity. One thread per guest CPU with the soft setting,
// one per guest core with the hard one.
void StepGuestCpuMapping(uint64_t core_mask) {
  const uint64_t guest_mask = rex::thread::nx_guest_core_mask();
  const uint64_t host_mask = rex::thread::nx_host_worker_core_mask();
  const bool masks_ok = guest_mask == (core_mask & 0x7) && host_mask == (core_mask & 0x8);
  SMOKE_INFO("  guest cores %s, host worker cores %s -> %s", Hex(guest_mask).c_str(),
             Hex(host_mask).c_str(), masks_ok ? "ok" : "MISMATCH");
  if (!masks_ok) {
    Fail("threads", "guest/host core masks " + Hex(guest_mask) + "/" + Hex(host_mask) +
                        " for process core mask " + Hex(core_mask));
  }
  // A host worker as the runtime will place it (04-threads-exit.md section 3):
  // ideal core 3, mask nx_host_worker_core_mask(). Only with core 3 (mask 0xF).
  if (host_mask) {
    std::atomic<int> host_ran{-1};
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 64 * 1024;
    params.create_suspended = true;
    auto worker = rex::thread::Thread::Create(params, [&host_ran] {
      uint64_t acc = 0;
      for (uint64_t k = 0; k < 1000000; k++) {
        acc = acc * 6364136223846793005ull + k;
      }
      host_ran.store(acc ? int(svcGetCurrentProcessorNumber()) : -2);
    });
    if (!worker) {
      Fail("threads", "Thread::Create failed for the host worker");
    } else {
      worker->set_ideal_core(3, host_mask);
      const int32_t ideal = worker->ideal_core();
      const uint64_t applied = worker->affinity_mask();
      worker->Resume();
      const bool joined = rex::thread::Wait(worker.get(), false, std::chrono::milliseconds(10000)) ==
                          rex::thread::WaitResult::kSuccess;
      const bool ok = joined && ideal == 3 && applied == host_mask && host_ran.load() == 3;
      SMOKE_INFO("  host worker: ideal %d, mask %s, ran on core %d -> %s", ideal,
                 Hex(applied).c_str(), host_ran.load(), ok ? "ok" : "WRONG");
      if (!ok) {
        Fail("threads", "host worker on core 3: ideal " + std::to_string(ideal) + ", mask " +
                            Hex(applied) + ", ran on core " + std::to_string(host_ran.load()));
      }
    }
  } else {
    SMOKE_INFO("  host worker: core 3 not in the process core mask, skipped");
  }

  const bool ignore = REXCVAR_GET(ignore_thread_affinities);
  SMOKE_INFO("  ignore_thread_affinities default %s -> %s", ignore ? "true" : "false",
             ignore ? "WRONG (must be false on NX)" : "ok");
  if (ignore) {
    Fail("threads", "ignore_thread_affinities defaults to true on NX");
  }

  struct Case {
    uint32_t cpu;
    bool hard;
  };
  std::vector<Case> cases;
  for (uint32_t cpu = 0; cpu < 6; ++cpu) {
    cases.push_back({cpu, false});
  }
  for (uint32_t cpu = 0; cpu < 6; cpu += 2) {
    cases.push_back({cpu, true});
  }
  std::vector<std::atomic<int>> ran(cases.size());
  std::atomic<uint64_t> work{0};  // keeps the busy loop from being optimised out
  std::vector<std::unique_ptr<rex::thread::Thread>> threads(cases.size());
  for (size_t n = 0; n < cases.size(); ++n) {
    const Case c = cases[n];
    const uint32_t expected_core = c.cpu / 2;
    const uint32_t core = rex::thread::nx_guest_cpu_core(c.cpu);
    ran[n].store(-1);
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 64 * 1024;
    params.create_suspended = true;
    auto thread = rex::thread::Thread::Create(params, [n, &ran, &work] {
      uint64_t acc = 0;
      for (uint64_t k = 0; k < 1000000; k++) {
        acc = acc * 6364136223846793005ull + k;
      }
      work.fetch_add(acc);
      ran[n].store(int(svcGetCurrentProcessorNumber()));
    });
    if (!thread) {
      Fail("threads", "Thread::Create failed for guest CPU " + std::to_string(c.cpu));
      continue;
    }
    const uint64_t mask = c.hard ? uint64_t(1) << core : guest_mask;
    thread->set_ideal_core(core, mask);
    const int32_t ideal = thread->ideal_core();
    const uint64_t applied = thread->affinity_mask();
    const bool ok = core == expected_core && ideal == int32_t(core) && applied == mask;
    SMOKE_INFO("  guest CPU %u %s: core %u (expected %u), ideal %d, mask %s -> %s", c.cpu,
               c.hard ? "hard" : "soft", core, expected_core, ideal, Hex(applied).c_str(),
               ok ? "ok" : "MISMATCH");
    if (!ok) {
      Fail("threads", "guest CPU " + std::to_string(c.cpu) + (c.hard ? " hard" : " soft") +
                          ": core " + std::to_string(core) + ", ideal " + std::to_string(ideal) +
                          ", mask " + Hex(applied));
    }
    thread->Resume();
    threads[n] = std::move(thread);
  }
  for (size_t n = 0; n < cases.size(); ++n) {
    if (!threads[n]) {
      continue;
    }
    if (rex::thread::Wait(threads[n].get(), false, std::chrono::milliseconds(10000)) !=
        rex::thread::WaitResult::kSuccess) {
      Fail("threads", "join timed out for guest CPU " + std::to_string(cases[n].cpu));
    }
    const Case c = cases[n];
    const int core = ran[n].load();
    const uint32_t expected = rex::thread::nx_guest_cpu_core(c.cpu);
    // Soft affinity may migrate within the guest cores; never onto core 3.
    const bool ok = core >= 0 && (guest_mask & (uint64_t(1) << core)) &&
                    (!c.hard || uint32_t(core) == expected);
    SMOKE_INFO("  guest CPU %u %s ran on core %d%s", c.cpu, c.hard ? "hard" : "soft", core,
               ok ? "" : " -> WRONG");
    if (!ok) {
      Fail("threads", "guest CPU " + std::to_string(c.cpu) + (c.hard ? " hard" : " soft") +
                          " ran on core " + std::to_string(core));
    }
  }
}

void StepThreads() {
  u64 core_mask = 0;
  svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  const uint32_t logical = rex::thread::logical_processor_count();
  const uint32_t expected_logical = uint32_t(__builtin_popcountll(core_mask));
  SMOKE_INFO("step 4 threads: logical processors %u, process core mask %s (%u cores) -> %s",
             logical, Hex(core_mask).c_str(), expected_logical,
             logical == expected_logical ? "ok" : "MISMATCH");
  if (logical != expected_logical) {
    Fail("threads", "logical_processor_count() = " + std::to_string(logical) + ", core mask has " +
                        std::to_string(expected_logical) + " cores");
  }
  rex::thread::EnableAffinityConfiguration();

  constexpr int kThreadCount = 4;
  std::atomic<uint64_t> results[kThreadCount] = {};
  std::atomic<int> cores[kThreadCount];
  for (auto& c : cores) c.store(-1);

  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  for (int i = 0; i < kThreadCount; i++) {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 256 * 1024;
    params.create_suspended = true;
    auto thread = rex::thread::Thread::Create(params, [i, &results, &cores] {
      uint64_t acc = 0;
      for (uint64_t k = 0; k < 2000000; k++) {
        acc = acc * 6364136223846793005ull + k + uint64_t(i);
      }
      results[i].store(acc | 1);
      cores[i].store(int(svcGetCurrentProcessorNumber()));
    });
    if (!thread) {
      Fail("threads", "Thread::Create failed for thread " + std::to_string(i));
      continue;
    }
    thread->set_name("smoke-" + std::to_string(i));
    if (core_mask & (1ull << i)) {
      thread->set_affinity_mask(1ull << i);
      const uint64_t applied = thread->affinity_mask();
      SMOKE_INFO("  thread %d -> core %d (affinity mask now %s)", i, i, Hex(applied).c_str());
      if (applied != (1ull << i)) {
        Fail("threads", "thread " + std::to_string(i) + " affinity mask " + Hex(applied) +
                            " after set_affinity_mask(" + Hex(1ull << i) + ")");
      }
    } else {
      SMOKE_INFO("  thread %d: core %d not in process core mask %s, default affinity", i, i,
                 Hex(core_mask).c_str());
    }
    thread->Resume();
    threads.push_back(std::move(thread));
  }

  for (size_t i = 0; i < threads.size(); i++) {
    auto result =
        rex::thread::Wait(threads[i].get(), false, std::chrono::milliseconds(10000));
    if (result != rex::thread::WaitResult::kSuccess) {
      Fail("threads", "join timed out or failed for thread " + std::to_string(i));
    }
  }
  for (int i = 0; i < kThreadCount; i++) {
    if (results[i].load() == 0) {
      Fail("threads", "thread " + std::to_string(i) + " did not finish its work");
    } else {
      SMOKE_INFO("  thread %d done on core %d", i, cores[i].load());
      if ((core_mask & (1ull << i)) && cores[i].load() != i) {
        Fail("threads", "thread " + std::to_string(i) + " pinned to core " + std::to_string(i) +
                            " ran on core " + std::to_string(cores[i].load()));
      }
    }
  }

  StepGuestCpuMapping(core_mask);
  StepThreadPriorities(core_mask);
}

}  // namespace

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  std::printf("Theft4 switch-smoke\n\n");
  consoleUpdate(nullptr);

  StepLogging();
  StepModuleBase();
  StepAddressSpace();
  StepGuestMemory();
  StepThreads();

  if (g_first_failure.empty()) {
    Report(false, "SMOKE OK");
  } else {
    Report(true, "SMOKE FAIL at %s", g_first_failure.c_str());
  }

  const bool passed_before_shutdown = g_first_failure.empty();
  RunProbes(g_memory.get());
  StepGuestMemoryShutdown();
  if (passed_before_shutdown && !g_first_failure.empty()) {
    // The verdict above came before the arena was released.
    Report(true, "SMOKE FAIL at %s", g_first_failure.c_str());
  }
  std::printf("\nPress + to exit.\n");
  consoleUpdate(nullptr);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      break;
    }
    consoleUpdate(nullptr);
  }

  rex::ShutdownLogging();
  consoleExit(nullptr);
  return 0;
}
