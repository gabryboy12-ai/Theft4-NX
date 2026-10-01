// The exit sequence of switch-game, shared with switch-smoke so the smoke
// tests the same code (06-game-boot-plan.md section 17):
//
//   1. PauseThreads: every given thread but the caller is paused with
//      svcSetThreadActivity(ThreadActivity_Paused), each result code kept,
//      and its context read back (svcGetThreadContext3 succeeds only on a
//      thread that is not running) as the check that it stopped;
//   2. RunOnExitWorker: the summary runs on a thread created at start-up and
//      is abandoned at its deadline. A paused thread may hold a lock the
//      summary needs (malloc, the log file, spdlog, the console), and the
//      exit must not depend on it;
//   3. the caller ends the process (svcExitProcess).
//
// PauseThreads takes no lock and does not allocate.

#pragma once

#include <cstddef>
#include <cstdint>

#include <switch.h>

namespace theft4::nx_exit {

struct PausedThread {
  Handle handle = 0;
  u64 thread_id = 0;      // 0 if svcGetThreadId failed (thread gone?)
  Result pause_rc = 0;    // svcSetThreadActivity(Paused)
  Result context_rc = 0;  // svcGetThreadContext3 after the pause: 0 = stopped
  u64 pc = 0;             // from that context
  // Stopped: its context could be read. pause_rc can fail on a thread that
  // was already paused (and is then still stopped).
  bool stopped() const { return R_SUCCEEDED(context_rc); }
};

// Pauses handles[0..count) except the calling thread and duplicates (by
// handle). Writes one entry per thread acted on into out (at most capacity)
// and returns how many.
size_t PauseThreads(const Handle* handles, size_t count, PausedThread* out, size_t capacity);

// Undoes a pause (switch-smoke only: the game never resumes).
void ResumeThreads(const PausedThread* threads, size_t count);

// Creates the worker (128 KiB stack, Horizon priority 0x2C, any core). Call
// at start-up, while allocating is still safe. false = not created.
bool StartExitWorker();

// Runs job(arg) on the worker and waits for it at most timeout_ms. true = it
// finished; false = deadline passed (it keeps running, or blocked) or the
// worker does not exist. One job at a time; a job still running from an
// earlier call makes this return false at once.
bool RunOnExitWorker(void (*job)(void*), void* arg, u64 timeout_ms);

}  // namespace theft4::nx_exit
