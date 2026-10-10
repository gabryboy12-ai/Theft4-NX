// switch-game: the console's clocks (nx_clocks.h). The configuration table
// and the lessons on when to read the clocks back come from nfsmw-nx
// (sdk/src/ui/switch_apm.cpp, BSD-3-Clause, Copyright (c) 2026 Tom Clay):
// apm hands the change to pcv, which applies it between 0.25 and ~1 s later,
// so the result is polled for 1.5 s instead of read once; 0x92220007 is the
// sibling with memory at 1600 MHz, which is not wanted (more heat, no gain).

#include "nx_clocks.h"

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>

#include <switch.h>

namespace theft4::nx_clocks {

namespace {

constexpr uint32_t kHandheldConfig = 0x92220008u;  // CPU 1020, GPU 460.8, memory 1331.2
constexpr uint32_t kWantedGpuHz = 460800000u;
constexpr uint32_t kStandardCpuHz = 1020000000u;
constexpr uint32_t kMarginHz = 20000000u;
constexpr uint64_t kPollStepNs = 150'000'000ull;
constexpr int kPollSteps = 10;  // 1.5 s

LogFn g_log = nullptr;
bool g_apply = false;
std::atomic<uint32_t> g_requested{0};
Thread g_thread;
UEvent g_mode_changed;
AppletHookCookie g_hook;
bool g_apm_ready = false;

void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Log(bool error, const char* fmt, ...) {
  char text[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  if (g_log) {
    g_log(error, text);
  }
}

uint32_t Distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

bool ReadRate(PcvModuleId module, uint32_t* hz) {
  ClkrstSession session{};
  if (R_FAILED(clkrstOpenSession(&session, module, 3))) {
    return false;
  }
  const bool ok = R_SUCCEEDED(clkrstGetClockRate(&session, hz));
  clkrstCloseSession(&session);
  return ok;
}

const char* ModeName(int mode) {
  return mode == ApmPerformanceMode_Normal  ? "handheld"
         : mode == ApmPerformanceMode_Boost ? "docked"
                                            : "mode unknown";
}

// Applied once at start-up and on every dock/handheld change.
void Apply(const char* why) {
  const Clocks before = Read();
  if (!g_apply) {
    Log(false, "clocks (%s): %s; nothing requested (cvar nx_apm_clock = false)", why,
        Describe(before).c_str());
    return;
  }
  if (before.mode != ApmPerformanceMode_Normal) {
    Log(false, "clocks (%s): %s; docked or unknown mode: nothing requested, the system's "
               "configuration stays", why, Describe(before).c_str());
    return;
  }
  if (!before.ok) {
    Log(true, "clocks (%s): clkrst cannot read the clocks: nothing requested, because the "
              "result could not be checked", why);
    return;
  }
  const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, kHandheldConfig);
  if (R_FAILED(rc)) {
    Log(true, "clocks (%s): apmSetPerformanceConfiguration(handheld, 0x%08X) -> 0x%X: NOT "
              "applied; %s", why, kHandheldConfig, rc, Describe(before).c_str());
    return;
  }
  g_requested.store(kHandheldConfig, std::memory_order_relaxed);
  // Memory must stay where it was for the whole poll (nfsmw-nx: one early read
  // once said 1331.2, and a second later it was 1600).
  Clocks after = before;
  bool memory_moved = false;
  for (int i = 0; i < kPollSteps; ++i) {
    svcSleepThread(kPollStepNs);
    after = Read();
    if (after.ok && Distance(after.emc_hz, before.emc_hz) > kMarginHz) {
      memory_moved = true;
      break;
    }
  }
  if (memory_moved) {
    const Result back =
        before.config ? apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, before.config)
                      : Result(-1);
    Log(true, "clocks (%s): 0x%08X moved the memory clock (%.1f -> %.1f MHz): back to 0x%08X "
              "(rc 0x%X); %s", why, kHandheldConfig, before.emc_hz / 1e6, after.emc_hz / 1e6,
        before.config, back, Describe(Read()).c_str());
    g_requested.store(0, std::memory_order_relaxed);
    return;
  }
  if (!after.ok || Distance(after.gpu_hz, kWantedGpuHz) > kMarginHz) {
    Log(true, "clocks (%s): requested 0x%08X (GPU 460.8 MHz) but NOT applied: %s", why,
        kHandheldConfig, Describe(after).c_str());
    return;
  }
  Log(false, "clocks (%s): applied 0x%08X, GPU %.1f -> %.1f MHz, memory %.1f MHz unchanged for "
             "1.5 s; %s", why, kHandheldConfig, before.gpu_hz / 1e6, after.gpu_hz / 1e6,
      after.emc_hz / 1e6, Describe(after).c_str());
}

void ClockThread(void*) {
  Apply("start-up");
  for (;;) {
    if (R_SUCCEEDED(waitSingle(waiterForUEvent(&g_mode_changed), UINT64_MAX))) {
      Apply("dock/handheld change");
    }
  }
}

// On the thread that runs appletMainLoop (the UI thread): only a signal.
void OnAppletHook(AppletHookType hook, void*) {
  if (hook == AppletHookType_OnPerformanceMode) {
    ueventSignal(&g_mode_changed);
  }
}

}  // namespace

Clocks Read() {
  Clocks c;
  if (R_SUCCEEDED(clkrstInitialize())) {
    c.ok = ReadRate(PcvModuleId_CpuBus, &c.cpu_hz) && ReadRate(PcvModuleId_GPU, &c.gpu_hz) &&
           ReadRate(PcvModuleId_EMC, &c.emc_hz);
    clkrstExit();
  }
  if (g_apm_ready) {
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    if (R_SUCCEEDED(apmGetPerformanceMode(&mode))) {
      c.mode = int(mode);
      u32 config = 0;
      if (mode != ApmPerformanceMode_Invalid &&
          R_SUCCEEDED(apmGetPerformanceConfiguration(mode, &config))) {
        c.config = config;
      }
    }
  }
  return c;
}

std::string Describe(const Clocks& c) {
  char text[384];
  if (!c.ok) {
    std::snprintf(text, sizeof(text), "clocks not readable (clkrst); %s, apm configuration 0x%08X",
                  ModeName(c.mode), c.config);
    return text;
  }
  const char* warning = "";
  if (c.gpu_hz && c.gpu_hz < 100000000u) {
    warning = "; GPU UNDER 100 MHz (CPU boost mode: the GPU drops to 76.8 MHz)";
  } else if (c.cpu_hz > kStandardCpuHz + kMarginHz) {
    warning = "; CPU ABOVE 1020 MHz (boost mode or an external overclock)";
  }
  const uint32_t requested = g_requested.load(std::memory_order_relaxed);
  char requested_text[48] = "";
  if (requested) {
    std::snprintf(requested_text, sizeof(requested_text), " (requested 0x%08X)", requested);
  }
  std::snprintf(text, sizeof(text),
                "CPU %.1f MHz, GPU %.1f MHz, memory %.1f MHz; %s, apm configuration 0x%08X%s%s",
                c.cpu_hz / 1e6, c.gpu_hz / 1e6, c.emc_hz / 1e6, ModeName(c.mode), c.config,
                requested_text, warning);
  return text;
}

uint32_t Requested() { return g_requested.load(std::memory_order_relaxed); }

void Start(bool apply, LogFn log) {
  g_log = log;
  g_apply = apply;
  const Result rc = apmInitialize();
  g_apm_ready = R_SUCCEEDED(rc);
  if (!g_apm_ready) {
    Log(true, "clocks: apmInitialize -> 0x%X: the mode and configuration are not read, nothing "
              "is requested", rc);
  }
  ueventCreate(&g_mode_changed, true);
  appletHook(&g_hook, OnAppletHook, nullptr);
  if (!g_apm_ready) {
    return;
  }
  // Priority 0x2C as the log writer; it sleeps on the event almost always.
  if (R_FAILED(threadCreate(&g_thread, ClockThread, nullptr, nullptr, 0x8000, 0x2C, -2)) ||
      R_FAILED(threadStart(&g_thread))) {
    Log(true, "clocks: the clock thread did not start: nothing requested");
  }
}

}  // namespace theft4::nx_clocks
