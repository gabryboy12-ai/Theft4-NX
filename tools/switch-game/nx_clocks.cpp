// switch-game: the console's clocks (nx_clocks.h). The configuration table
// and the lessons on when to read the clocks back come from nfsmw-nx
// (sdk/src/ui/switch_apm.cpp, BSD-3-Clause, Copyright (c) 2026 Tom Clay):
// apm hands the change to pcv, which applies it between 0.25 and ~1 s later,
// so the result is polled for 1.5 s instead of read once; 0x92220007 is the
// sibling with memory at 1600 MHz, which is not wanted (more heat, no gain).

#include "nx_clocks.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include <switch.h>

namespace theft4::nx_clocks {

namespace {

constexpr uint32_t kHandheldConfig = 0x92220008u;  // CPU 1020, GPU 460.8, memory 1331.2
constexpr uint32_t kWantedGpuHz = 460800000u;
constexpr uint32_t kStandardCpuHz = 1020000000u;
constexpr uint32_t kMarginHz = 20000000u;
constexpr uint64_t kPollStepNs = 150'000'000ull;
constexpr int kPollSteps = 10;  // 1.5 s
constexpr uint64_t kCheckNs = 5'000'000'000ull;
constexpr int kHotMilliC = 80000;

// Nintendo's clock steps (pcv), up to the highest any game is given: CPU 1785
// MHz in boost mode, GPU 768 MHz docked, memory 1600 MHz.
constexpr uint32_t kCpuSteps[] = {1020000000u, 1224000000u, 1326000000u, 1428000000u,
                                  1581000000u, 1683000000u, 1785000000u};
constexpr uint32_t kGpuSteps[] = {307200000u, 384000000u, 460800000u, 537600000u,
                                  614400000u, 691200000u, 768000000u};
constexpr uint32_t kEmcSteps[] = {1065600000u, 1331200000u, 1600000000u};

LogFn g_log = nullptr;
bool g_apply = false;
std::atomic<uint32_t> g_requested{0};
Thread g_thread;
UEvent g_mode_changed;
AppletHookCookie g_hook;
bool g_apm_ready = false;

// Raising (nx_cpu_mhz, nx_gpu_mhz, nx_emc_mhz), all on the clock thread
// except RestoreAtExit.
uint32_t g_target_cpu_hz = 0, g_target_gpu_hz = 0, g_target_emc_hz = 0;
bool g_mariko = false;
int g_product_model = -1;
Clocks g_base;                      // the configuration's clocks, restored at exit
std::atomic<bool> g_raised{false};  // g_raised_* are what the thread keeps
uint32_t g_raised_cpu_hz = 0, g_raised_gpu_hz = 0, g_raised_emc_hz = 0;
bool g_too_hot = false;
uint32_t g_raised_again = 0;
bool g_atexit_registered = false;

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

bool SetRate(PcvModuleId module, uint32_t hz) {
  if (R_FAILED(clkrstInitialize())) {
    return false;
  }
  bool ok = false;
  ClkrstSession session{};
  if (R_SUCCEEDED(clkrstOpenSession(&session, module, 3))) {
    ok = R_SUCCEEDED(clkrstSetClockRate(&session, hz));
    clkrstCloseSession(&session);
  }
  clkrstExit();
  return ok;
}

// The step nearest to `mhz` (0 = none).
template <size_t N>
uint32_t NearestStep(const uint32_t (&steps)[N], int32_t mhz) {
  if (mhz <= 0) {
    return 0;
  }
  const uint64_t hz = uint64_t(mhz) * 1000000u;
  uint32_t best = steps[0];
  for (uint32_t step : steps) {
    const uint64_t d = step > hz ? step - hz : hz - step;
    const uint64_t best_d = best > hz ? best - hz : hz - best;
    if (d < best_d) {
      best = step;
    }
  }
  return best;
}

int ReadSocMilliC() {
  if (R_FAILED(tsInitialize())) {
    return 0;
  }
  int milli_c = 0;
  if (hosversionAtLeast(10, 0, 0)) {
    TsSession session{};
    if (R_SUCCEEDED(tsOpenSession(&session, TsDeviceCode_LocationExternal))) {
      float c = 0;
      if (R_SUCCEEDED(tsSessionGetTemperature(&session, &c))) {
        milli_c = int(c * 1000.0f);
      }
      tsSessionClose(&session);
    }
  } else {
    s32 c = 0;
    if (R_SUCCEEDED(tsGetTemperature(TsLocation_External, &c))) {
      milli_c = int(c) * 1000;
    }
  }
  tsExit();
  return milli_c;
}

const char* ModeName(int mode) {
  return mode == ApmPerformanceMode_Normal  ? "handheld"
         : mode == ApmPerformanceMode_Boost ? "docked"
                                            : "mode unknown";
}

const char* ProductModelName(int model) {
  switch (model) {
    case SetSysProductModel_Nx: return "Erista";
    case SetSysProductModel_Copper: return "Erista (Copper)";
    case SetSysProductModel_Iowa: return "Mariko";
    case SetSysProductModel_Hoag: return "Mariko Lite";
    case SetSysProductModel_Calcio: return "Mariko (Calcio)";
    case SetSysProductModel_Aula: return "Mariko OLED";
    default: return "model unknown";
  }
}

// The configuration's clocks back (at exit, or when the SoC is too hot).
void RestoreBase() {
  if (!g_raised.exchange(false) || !g_base.ok) {
    return;
  }
  SetRate(PcvModuleId_CpuBus, g_base.cpu_hz);
  SetRate(PcvModuleId_GPU, g_base.gpu_hz);
  SetRate(PcvModuleId_EMC, g_base.emc_hz);
}

void RestoreAtExit() { RestoreBase(); }

// Applied once at start-up and on every dock/handheld change.
void ApplyConfiguration(const char* why) {
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

// On top of the configuration: the nx_*_mhz clocks, never below what the
// configuration set.
void Raise(const char* why) {
  if (!g_target_cpu_hz && !g_target_gpu_hz && !g_target_emc_hz) {
    return;
  }
  if (g_too_hot) {
    Log(true, "clocks (%s): not raised: the SoC reached %d C earlier in this run", why,
        kHotMilliC / 1000);
    return;
  }
  g_raised.store(false);
  const Clocks base = Read();
  if (!base.ok) {
    Log(true, "clocks (%s): clkrst cannot read the clocks: not raised", why);
    return;
  }
  g_base = base;
  // GPU above 460.8 MHz in handheld mode only on Mariko (docked clocks on
  // battery); Erista or an unknown model stays at the handheld clock.
  uint32_t gpu_target = g_target_gpu_hz;
  if (base.mode != ApmPerformanceMode_Boost && !g_mariko) {
    gpu_target = std::min(gpu_target, kWantedGpuHz);
  }
  g_raised_cpu_hz = std::max(base.cpu_hz, g_target_cpu_hz);
  g_raised_gpu_hz = std::max(base.gpu_hz, gpu_target);
  g_raised_emc_hz = std::max(base.emc_hz, g_target_emc_hz);
  bool ok = true;
  if (Distance(g_raised_emc_hz, base.emc_hz) > kMarginHz) {
    ok &= SetRate(PcvModuleId_EMC, g_raised_emc_hz);
  }
  if (Distance(g_raised_gpu_hz, base.gpu_hz) > kMarginHz) {
    ok &= SetRate(PcvModuleId_GPU, g_raised_gpu_hz);
  }
  if (Distance(g_raised_cpu_hz, base.cpu_hz) > kMarginHz) {
    ok &= SetRate(PcvModuleId_CpuBus, g_raised_cpu_hz);
  }
  g_raised.store(true);
  if (!g_atexit_registered) {
    g_atexit_registered = true;
    std::atexit(RestoreAtExit);
  }
  svcSleepThread(300'000'000ull);
  const Clocks after = Read();
  const bool held = after.ok && Distance(after.cpu_hz, g_raised_cpu_hz) <= kMarginHz &&
                    Distance(after.gpu_hz, g_raised_gpu_hz) <= kMarginHz &&
                    Distance(after.emc_hz, g_raised_emc_hz) <= kMarginHz;
  Log(!ok || !held,
      "clocks (%s): raised to CPU %.1f, GPU %.1f, memory %.1f MHz (cvars nx_cpu_mhz, "
      "nx_gpu_mhz, nx_emc_mhz; %s, GPU above 460.8 MHz in handheld mode %s): clkrst %s, read "
      "back %s; %s",
      why, g_raised_cpu_hz / 1e6, g_raised_gpu_hz / 1e6, g_raised_emc_hz / 1e6,
      ProductModelName(g_product_model), g_mariko ? "allowed" : "not allowed",
      ok ? "ok" : "FAILED", held ? "as set" : "DIFFERENT", Describe(after).c_str());
}

void Apply(const char* why) {
  ApplyConfiguration(why);
  Raise(why);
}

// Every 5 s: raise again what the system put back, and give up at 80 C.
void Check() {
  if (!g_raised.load()) {
    return;
  }
  const Clocks c = Read();
  if (c.soc_milli_c >= kHotMilliC) {
    g_too_hot = true;
    RestoreBase();
    Log(true, "clocks: SoC at %.1f C: back to the configuration's clocks for the rest of the "
              "run; %s", c.soc_milli_c / 1000.0, Describe(Read()).c_str());
    return;
  }
  if (!c.ok || (Distance(c.cpu_hz, g_raised_cpu_hz) <= kMarginHz &&
                Distance(c.gpu_hz, g_raised_gpu_hz) <= kMarginHz &&
                Distance(c.emc_hz, g_raised_emc_hz) <= kMarginHz)) {
    return;
  }
  ++g_raised_again;
  if (g_raised_again <= 5 || !(g_raised_again % 100)) {
    Log(false, "clocks: the system set CPU %.1f, GPU %.1f, memory %.1f MHz: raising again (%u "
               "times so far)", c.cpu_hz / 1e6, c.gpu_hz / 1e6, c.emc_hz / 1e6, g_raised_again);
  }
  SetRate(PcvModuleId_EMC, g_raised_emc_hz);
  SetRate(PcvModuleId_GPU, g_raised_gpu_hz);
  SetRate(PcvModuleId_CpuBus, g_raised_cpu_hz);
}

void ClockThread(void*) {
  Apply("start-up");
  for (;;) {
    if (R_SUCCEEDED(waitSingle(waiterForUEvent(&g_mode_changed), kCheckNs))) {
      Apply("dock/handheld change");
    } else {
      Check();
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
  c.soc_milli_c = ReadSocMilliC();
  return c;
}

std::string Describe(const Clocks& c) {
  char text[384];
  char soc[32] = "";
  if (c.soc_milli_c) {
    std::snprintf(soc, sizeof(soc), ", SoC %.1f C", c.soc_milli_c / 1000.0);
  }
  if (!c.ok) {
    std::snprintf(text, sizeof(text),
                  "clocks not readable (clkrst); %s, apm configuration 0x%08X%s",
                  ModeName(c.mode), c.config, soc);
    return text;
  }
  const char* warning = "";
  if (c.gpu_hz && c.gpu_hz < 100000000u) {
    warning = "; GPU UNDER 100 MHz (CPU boost mode: the GPU drops to 76.8 MHz)";
  } else if (c.cpu_hz > kStandardCpuHz + kMarginHz && !g_raised.load()) {
    warning = "; CPU ABOVE 1020 MHz (boost mode or an external overclock)";
  }
  const uint32_t requested = g_requested.load(std::memory_order_relaxed);
  char requested_text[48] = "";
  if (requested) {
    std::snprintf(requested_text, sizeof(requested_text), " (requested 0x%08X)", requested);
  }
  std::snprintf(text, sizeof(text),
                "CPU %.1f MHz, GPU %.1f MHz, memory %.1f MHz%s; %s, apm configuration 0x%08X%s%s%s",
                c.cpu_hz / 1e6, c.gpu_hz / 1e6, c.emc_hz / 1e6, soc, ModeName(c.mode), c.config,
                requested_text, g_raised.load() ? ", raised by nx_*_mhz" : "", warning);
  return text;
}

uint32_t Requested() { return g_requested.load(std::memory_order_relaxed); }

void SetTargets(int32_t cpu_mhz, int32_t gpu_mhz, int32_t emc_mhz) {
  g_target_cpu_hz = NearestStep(kCpuSteps, cpu_mhz);
  g_target_gpu_hz = NearestStep(kGpuSteps, gpu_mhz);
  g_target_emc_hz = NearestStep(kEmcSteps, emc_mhz);
}

void Start(bool apply, LogFn log) {
  g_log = log;
  g_apply = apply;
  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysProductModel model = SetSysProductModel_Invalid;
    if (R_SUCCEEDED(setsysGetProductModel(&model))) {
      g_product_model = int(model);
      g_mariko = model == SetSysProductModel_Iowa || model == SetSysProductModel_Hoag ||
                 model == SetSysProductModel_Calcio || model == SetSysProductModel_Aula;
    }
    setsysExit();
  }
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
