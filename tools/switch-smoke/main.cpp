// switch-smoke: on-device smoke test for the ReXGlue SDK on Nintendo Switch.
//
// Steps, in order; each prints its outcome on screen and in
// sdmc:/switch/theft4/smoke.log (through the SDK logger):
//   1. SDK logging
//   2. svcGetInfo address-space layout (alias, heap, ASLR regions)
//   3. guest memory reservation, done exactly like
//      rex::system::Memory::Initialize() (src/system/xmemory.cpp): same
//      mapping size, same CreateFileMappingHandle / MapFileView calls from
//      memory_switch.cpp, then a write/read-back at both ends of the region
//   4. four threads through rex::thread::Thread, one per core where allowed
// The last line is "SMOKE OK" or "SMOKE FAIL at <step>: <reason>". Press + to
// exit.

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

#include <sys/stat.h>

#include <switch.h>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace {

constexpr const char* kLogDir = "sdmc:/switch/theft4";
constexpr const char* kLogPath = "sdmc:/switch/theft4/smoke.log";

std::string g_first_failure;
bool g_logging_ready = false;

void Report(bool error, const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  std::printf("%s\n", line);
  consoleUpdate(nullptr);
  if (g_logging_ready) {
    if (error) {
      REXLOG_ERROR("[smoke] {}", line);
    } else {
      REXLOG_INFO("[smoke] {}", line);
    }
    rex::FlushLogging();
  }
}

#define SMOKE_INFO(...) Report(false, __VA_ARGS__)

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
  rex::InitLogging(kLogPath, spdlog::level::debug);
  g_logging_ready = true;
  SMOKE_INFO("step 1 logging: SDK logger writing to %s", kLogPath);
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
}

// ── 3. Guest memory ─────────────────────────────────────────────────────────

// Copy of map_info[] in src/system/xmemory.cpp (static there).
struct MapInfo {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
};
constexpr MapInfo kMapInfo[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull},
    {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull},
    {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull},
    {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull},
    {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},
};
constexpr size_t kViewCount = sizeof(kMapInfo) / sizeof(kMapInfo[0]);

void UnmapViews(rex::memory::FileMappingHandle mapping, uint8_t* (&views)[kViewCount]) {
  for (size_t n = 0; n < kViewCount; n++) {
    if (views[n]) {
      rex::memory::UnmapFileView(
          mapping, views[n], kMapInfo[n].virtual_address_end - kMapInfo[n].virtual_address_start + 1);
      views[n] = nullptr;
    }
  }
}

// Memory::MapViews().
int MapViews(rex::memory::FileMappingHandle mapping, uint8_t* mapping_base, size_t granularity,
             uint8_t* (&views)[kViewCount]) {
  const uint64_t granularity_mask = ~uint64_t(granularity - 1);
  for (size_t n = 0; n < kViewCount; n++) {
    views[n] = reinterpret_cast<uint8_t*>(rex::memory::MapFileView(
        mapping, mapping_base + kMapInfo[n].virtual_address_start,
        kMapInfo[n].virtual_address_end - kMapInfo[n].virtual_address_start + 1,
        rex::memory::PageAccess::kReadWrite, kMapInfo[n].target_address & granularity_mask));
    if (!views[n]) {
      UnmapViews(mapping, views);
      return 1;
    }
  }
  return 0;
}

bool WriteReadBack(const char* label, uint8_t* address, uint64_t pattern) {
  volatile uint64_t* p = reinterpret_cast<volatile uint64_t*>(address);
  *p = pattern;
  const uint64_t read = *p;
  SMOKE_INFO("  %s %p: wrote %s read %s -> %s", label, static_cast<void*>(address),
             Hex(pattern).c_str(), Hex(read).c_str(), read == pattern ? "ok" : "MISMATCH");
  return read == pattern;
}

// Reserves like Memory::Initialize(), maps the views like Memory::MapViews()
// and touches both ends of the physically backed region.
bool ReserveAndProbe(const char* step, size_t mapping_size) {
  const size_t granularity = rex::memory::allocation_granularity();
  const std::string file_name =
      "xenia_memory_" + std::to_string(rex::chrono::Clock::QueryHostTickCount());

  SMOKE_INFO("  CreateFileMappingHandle(size=%s, kReadWrite, commit=false)",
             Hex(mapping_size).c_str());
  const auto mapping = rex::memory::CreateFileMappingHandle(
      file_name, mapping_size, rex::memory::PageAccess::kReadWrite, false);
  if (mapping == rex::memory::kFileMappingHandleInvalid) {
    Fail(step, "CreateFileMappingHandle(" + Hex(mapping_size) +
                   ") returned kFileMappingHandleInvalid (see log for memory_switch reason)");
    return false;
  }

  uint8_t* views[kViewCount] = {};
  uint8_t* mapping_base = nullptr;
  for (size_t n = 32; n < 64; n++) {
    auto candidate = reinterpret_cast<uint8_t*>(1ull << n);
    if (!MapViews(mapping, candidate, granularity, views)) {
      mapping_base = candidate;
      break;
    }
  }
  bool ok = true;
  if (!mapping_base) {
    Fail(step, "MapViews failed for every base 1<<32 .. 1<<63");
    ok = false;
  } else {
    SMOKE_INFO("  MapViews ok at mapping_base %p", static_cast<void*>(mapping_base));
    for (size_t n = 0; n < kViewCount; n++) {
      SMOKE_INFO("    view %zu guest %s-%s -> host %p", n,
                 Hex(kMapInfo[n].virtual_address_start).c_str(),
                 Hex(kMapInfo[n].virtual_address_end).c_str(), static_cast<void*>(views[n]));
    }
    if (views[0] != mapping_base) {
      SMOKE_INFO(
          "  note: primary view is at %p, not at mapping_base %p; the runtime uses "
          "mapping_base as virtual_membase_",
          static_cast<void*>(views[0]), static_cast<void*>(mapping_base));
    }

    // views[0] is the physically backed store (memory_switch.cpp commits the
    // whole mapping there). Probe its first and last 8 bytes; aliased views
    // are unbacked on Switch and are not touched.
    size_t backed_size = 0;
    rex::memory::PageAccess access{};
    size_t query_length = 0;
    if (rex::memory::QueryProtect(views[0], query_length, access)) {
      backed_size = query_length;
    }
    const size_t aligned_size = (mapping_size + granularity - 1) & ~(granularity - 1);
    SMOKE_INFO("  backing store %p size %s (QueryProtect region %s)", static_cast<void*>(views[0]),
               Hex(aligned_size).c_str(), Hex(backed_size).c_str());
    ok &= WriteReadBack("first", views[0], 0x5EED0000CAFEF00Dull);
    ok &= WriteReadBack("last ", views[0] + aligned_size - sizeof(uint64_t), 0xDEADBEEF01234567ull);
    if (!ok) {
      Fail(step, "write/read-back mismatch");
    }
    UnmapViews(mapping, views);
  }
  rex::memory::CloseFileMappingHandle(mapping, file_name);
  return ok;
}

void StepGuestMemory() {
  // Memory::Initialize(): round_up(0x120000000 + granularity, granularity).
  const size_t granularity = rex::memory::allocation_granularity();
  const size_t runtime_size =
      (size_t(0x120000000ull) + granularity + granularity - 1) & ~(granularity - 1);
  SMOKE_INFO("step 3 guest memory (runtime path, granularity %s)", Hex(granularity).c_str());
  if (ReserveAndProbe("guest memory", runtime_size)) {
    SMOKE_INFO("  runtime-size reservation ok");
    return;
  }

  // Diagnostic only, not the runtime path: the largest request that
  // memory_switch.cpp accepts (kNxMaxMappingBytes = 2304 MiB), to learn
  // whether that budget is actually available on this console.
  constexpr size_t kNxMaxMappingBytes = size_t(2304) * 1024 * 1024;
  SMOKE_INFO("step 3b DIAGNOSTIC: retry at the memory_switch.cpp cap %s",
             Hex(kNxMaxMappingBytes).c_str());
  if (ReserveAndProbe("guest memory diagnostic", kNxMaxMappingBytes)) {
    SMOKE_INFO("  diagnostic reservation at the cap ok");
  }
}

// ── 4. Threads ──────────────────────────────────────────────────────────────

void StepThreads() {
  SMOKE_INFO("step 4 threads (logical processors reported: %u)",
             rex::thread::logical_processor_count());
  u64 core_mask = 0;
  svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
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
      SMOKE_INFO("  thread %d -> core %d", i, i);
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
  StepAddressSpace();
  StepGuestMemory();
  StepThreads();

  if (g_first_failure.empty()) {
    Report(false, "SMOKE OK");
  } else {
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
