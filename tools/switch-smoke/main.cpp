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
//      must match the mask
// Then "SMOKE OK" or "SMOKE FAIL at <step>: <reason>" for those four steps,
// followed by the exploratory probes T1-T9 in probes.cpp (alias primitives,
// heap size, cost of the guest-memory design A). Press + to exit.
//
// Every smoke line is made durable before the next operation: the SDK logger
// is flushed, then the line is appended to smoke.log through its own fd,
// fsync'd and closed. A crash therefore leaves the last attempted operation
// as the last line of the log.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/chrono/clock.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "probes.h"

namespace {

constexpr const char* kLogDir = "sdmc:/switch/theft4";
constexpr const char* kLogPath = "sdmc:/switch/theft4/smoke.log";

std::string g_first_failure;
bool g_logging_ready = false;

}  // namespace

// Start of the NRO image; switch.ld places it at ELF address 0, so its runtime
// address is the module base.
extern "C" char __start__;
int main(int argc, char** argv);

namespace {

// Appends one line to smoke.log and forces it to storage: open, write, fsync,
// close. The SDK logger is flushed first so its lines stay in order.
void DurableAppend(const char* line) {
  rex::FlushLogging();
  int fd = open(kLogPath, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd < 0) {
    return;
  }
  (void)write(fd, line, std::strlen(line));
  (void)write(fd, "\n", 1);
  fsync(fd);
  close(fd);
}

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
  rex::InitLogging(kLogPath, spdlog::level::debug);
  REXLOG_INFO("[smoke] SDK logger initialised");
  g_logging_ready = true;
  SMOKE_INFO("step 1 logging: SDK logger writing to %s", kLogPath);
  if (!policy_ok) {
    Fail("logging", "diagnostics::Configure(logging): " + policy_error);
  }
}

// Runtime addresses to match crash-report PCs against the ELF:
// ELF address = runtime address - module base.
void StepModuleBase() {
  const u64 module_base = reinterpret_cast<u64>(&__start__);
  const u64 main_address = reinterpret_cast<u64>(&main);
  MemoryInfo info{};
  u32 page_info = 0;
  const Result rc = svcQueryMemory(&info, &page_info, main_address);
  SMOKE_INFO("module base (__start__) 0x%" PRIx64 ", main 0x%" PRIx64 " = base + 0x%" PRIx64,
             module_base, main_address, main_address - module_base);
  SMOKE_INFO("  code block of main: 0x%" PRIx64 "+0x%" PRIx64 " (rc 0x%x)", info.addr, info.size,
             rc);
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
             "(%zu references), %" PRIu64 " maps / %" PRIu64 " unmaps, %.2f ms moving blocks in",
             when, stats.mapped_blocks, stats.mapped_blocks * 2, stats.peak_mapped_blocks,
             stats.committed_pages, stats.page_references, stats.map_calls, stats.unmap_calls,
             double(armTicksToNs(stats.map_ticks)) / 1e6);
  return stats;
}

// memory_switch.cpp trace (every SVC of the commit/protect path with its
// Result, and the failing step) into the smoke log, for the duration of a
// scope. Not used around faults: the sink would run in the exception handler.
void TraceLine(const char* line) {
  SMOKE_INFO("    trace %s", line);
}

struct ScopedMemoryTrace {
  ScopedMemoryTrace() { rex::memory::nx::SetMemoryTraceSink(TraceLine); }
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
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
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
