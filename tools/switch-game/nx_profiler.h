// switch-game: stack sampling of the busy threads (docs/switch-port/
// 06-game-boot-plan.md section 27, run 21, B.3). Off unless cvar nx_profile.
//
// The minimal form of the profiler in the shared SDK of sa2-nx and nfsmw-nx
// (sdk/src/ui/switch_perf.cpp, BSD-3-Clause, Copyright (c) 2026 Tom Clay;
// portions derived from Xenia, Copyright (c) 2022 Ben Vanik and Xenia project
// contributors): every millisecond a thread at priority 0x2A takes the next
// busy thread in turn, pauses it (svcSetThreadActivity), reads its pc, lr and
// a few return addresses through the x29 chain (svcGetThreadContext3) and
// resumes it. Between pause and resume there are only system calls and reads
// of the paused thread's own stack: nothing that takes a lock. A thread that
// is already paused (by the game or by a snapshot) is left alone.
//
// Every 10 s the profiler thread writes one line per sampled thread:
//   [game] profile id 18 guest 'XThreadB9A09080' 2210 samples: +0x357328 x120 ...
// pc offsets from the module base with their counts, which
// tools/switch-game/where.py --profile adds up per function.

#pragma once

#include <cstddef>
#include <cstdint>

namespace theft4::nx_profiler {

using LogFn = void (*)(const char* text);

struct BusyThread {
  uint32_t handle = 0;  // Horizon handle
  uint32_t id = 0;      // XThread id (0 = not an XThread)
  char kind[8] = {};    // guest, host, ui
  char name[24] = {};
  double percent = 0;   // of one core in the last heartbeat window
};

// Starts the sampling thread. Call once, after logging is up.
void Start(uint64_t module_base, uint64_t module_text_size, LogFn log);

bool Running();

// From the heartbeat (UI thread): the threads worth sampling now.
void SetBusyThreads(const BusyThread* threads, size_t count);

}  // namespace theft4::nx_profiler
