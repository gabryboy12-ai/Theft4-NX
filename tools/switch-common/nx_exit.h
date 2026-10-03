// Exit of switch-game and switch-smoke (06-game-boot-plan.md sections 17-19,
// 09-rules.md section 9).
//
// Run 15 settled it: under hbloader the process leaves with exit(0), never
// with svcExitProcess nor with the game's threads paused. Two modes remain:
//
//   A  exit(0), what returning from main does: atexit handlers, _exit,
//      __libnx_exit (__appExit, then back to hbloader); default of the smoke;
//   D  every thread is told to end and joined for at most 2 s, then exit(0)
//      as A; default of the game since run 18 (it also closes the file I/O
//      gate and stops the null audio worker first, 06 §23).
//
// PauseThreads and RunOnExitWorker are no longer on any exit path: only the
// smoke's pause test uses them (cvar nx_smoke_pause_test, off by default).
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

// ── Exit modes ─────────────────────────────────────────────────────────────

// Steps go to a file of their own, opened at start-up: each line is written
// and fsync'd on that descriptor (no allocation, no lock), so its last line
// says how far the exit got whatever the other threads hold.
bool OpenStepLog(const char* path);
// "exit mode <mode>: step <n>: <what>". Returns the line (static buffer,
// valid until the next call) for a copy in the main log.
const char* Step(char mode, int n, const char* what);

// "A" or "D" from the cvar nx_exit_mode; anything else (B and C included,
// removed after run 15) gives A.
char ParseExitMode(const char* value);
const char* ExitModeDescription(char mode);

}  // namespace theft4::nx_exit
