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
// left as the system sets it (GPU 768 MHz). No boost mode, no overclock: if
// the clocks read back are not the requested ones, the log says so. Applied
// on a thread of its own at start-up and again on every dock/handheld change
// (applet hook OnPerformanceMode).

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
};

// clkrst and apm, opened and closed on every call (a few times a minute).
Clocks Read();

// One heartbeat line: "clocks: CPU 1020.0 MHz, GPU 460.8 MHz, memory 1331.2
// MHz; handheld, apm configuration 0x92220008 (requested ...)".
std::string Describe(const Clocks& c);

// Starts the thread; apply = cvar nx_apm_clock. Call once, after logging is up.
void Start(bool apply, LogFn log);

// The configuration requested in handheld mode (0 = nothing requested).
uint32_t Requested();

}  // namespace theft4::nx_clocks
