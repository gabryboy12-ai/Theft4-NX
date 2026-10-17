// switch-game: the console's clocks (docs/switch-port/06-game-boot-plan.md
// section 27, run 21, C).
//
// Reading: clkrst (CPU, GPU, memory), apm (performance mode and
// configuration), as nfsmw-nx does (sdk/src/ui/switch_apm.cpp, BSD-3-Clause,
// Copyright (c) 2026 Tom Clay). The heartbeat prints one line every 10 s.
//
// Requesting: with cvar nx_apm_clock (default true), in handheld mode the
// standard configuration 0x92220008 (GPU 460.8 MHz, memory 1331.2 MHz, CPU
// 1020 MHz), which Horizon offers to every game through apm. Docked mode is
// left as the system sets it (GPU 768 MHz). Applied on a thread of its own at
// start-up and again on every dock/handheld change (applet hook
// OnPerformanceMode).
//
// Raising (run 3): on top of that configuration, clkrst sets the clocks of
// cvars nx_cpu_mhz, nx_gpu_mhz and nx_emc_mhz, as sys-clk does. Only
// Nintendo's own steps, at most CPU 1785 MHz (boost mode), GPU 768 MHz
// (docked) and memory 1600 MHz; a clock is never lowered, and the GPU stays at
// 460.8 MHz in handheld mode on Erista consoles. Run 2 at 1020/460.8/1331.2
// was CPU-bound in pipeline compilation and the game threads. The thread
// checks every 5 s and raises again what the system put back; the
// configuration's clocks come back at exit, and at 80 C (SoC) for good.

#pragma once

#include <cstdint>
#include <string>

namespace theft4::nx_clocks {

using LogFn = void (*)(bool error, const char* text);

struct Clocks {
  bool ok = false;  // clkrst answered
  uint32_t cpu_hz = 0, gpu_hz = 0, emc_hz = 0;
  int mode = -1;            // ApmPerformanceMode: 0 handheld, 1 docked, -1 unknown
  uint32_t config = 0;      // apm configuration of that mode (0 = not read)
  int soc_milli_c = 0;      // SoC temperature (ts), 0 = not read
};

// clkrst, apm and ts, opened and closed on every call (a few times a minute).
Clocks Read();

// One heartbeat line: "clocks: CPU 1020.0 MHz, GPU 460.8 MHz, memory 1331.2
// MHz; handheld, apm configuration 0x92220008 (requested ...)".
std::string Describe(const Clocks& c);

// Clocks to raise to in MHz (cvars nx_cpu_mhz, nx_gpu_mhz, nx_emc_mhz; 0 =
// keep the configuration's). Call before Start.
void SetTargets(int32_t cpu_mhz, int32_t gpu_mhz, int32_t emc_mhz);

// Starts the thread; apply = cvar nx_apm_clock. Call once, after logging is up.
void Start(bool apply, LogFn log);

// The configuration requested in handheld mode (0 = nothing requested).
uint32_t Requested();

}  // namespace theft4::nx_clocks
