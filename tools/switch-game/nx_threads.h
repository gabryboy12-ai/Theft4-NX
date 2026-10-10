// switch-game: every libnx thread of the process, its stack and its name
// (docs/switch-port/06-game-boot-plan.md section 27, run 23, C.3 and D.2).
//
// The link wraps threadCreate, threadClose and pthread_create (CMakeLists),
// so every thread is seen, Mesa's and NAK's (Rust) included: libnx's newlib
// glue creates pthreads with threadCreate. For each one the table keeps the
// stack range ([stack_mirror, stack_mirror + stack_sz), the top being the
// initial sp: libnx puts the TLS and reent above it), the start routine of a
// pthread, and a name the heartbeat fills in (XThreads, ui, log).
//
// Stack watch (cvar nx_stack_watch): a new thread's stack is filled with a
// pattern before it starts (the main thread's below its sp at start-up); the
// heartbeat scans each live stack from the bottom for the deepest word
// written and logs size and deepest use per thread.
//
// The crash record pieces (Find, WriteAllThreads) use only system calls and
// reads of the table: safe in the fault handler.

#pragma once

#include <cstddef>
#include <cstdint>

namespace theft4::nx_threads {

using LogFn = void (*)(const char* text);
using WriteFn = void (*)(const char* text);

// Before any thread is created by us: turns the stack pattern on or off
// (threads already created stay as they are) and records the main thread.
void Start(bool stack_watch, uint64_t module_base);

// Heartbeat: the name of a thread by its Horizon handle (XThreads etc.).
void SetName(uint32_t handle, const char* name);

// Heartbeat: one line per live thread (size, deepest use, start routine) and
// a summary line; scans the painted stacks.
void LogStacks(LogFn log);

struct Info {
  bool found = false;
  uint32_t handle = 0;
  uint64_t id = 0;
  uint64_t stack_lo = 0;  // lowest address of the stack
  uint64_t stack_hi = 0;  // initial sp (top)
  uint64_t start = 0;     // pthread start routine or libnx entry (absolute)
  char name[40] = {};
};

// Fault-safe: the thread with this Horizon thread id.
bool FindById(uint64_t id, Info* out);

// Fault-safe: [lo, hi) of the memory block holding `address`
// (svcQueryMemory), 0/0 if unmapped.
void QueryBlock(uint64_t address, uint64_t* lo, uint64_t* hi);

// Fault-safe: every other live thread paused for a moment
// (svcSetThreadActivity), its pc, lr and sp read (svcGetThreadContext3) and
// resumed; then one line per thread through `write`.
void WriteAllThreads(WriteFn write, uint64_t self_id);

}  // namespace theft4::nx_threads
