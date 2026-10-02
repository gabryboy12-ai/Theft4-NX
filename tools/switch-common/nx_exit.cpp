#include "nx_exit.h"

#include <atomic>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace theft4::nx_exit {

size_t PauseThreads(const Handle* handles, size_t count, PausedThread* out, size_t capacity) {
  u64 self_id = 0;
  svcGetThreadId(&self_id, CUR_THREAD_HANDLE);
  size_t n = 0;
  for (size_t i = 0; i < count && n < capacity; ++i) {
    const Handle h = handles[i];
    if (!h) {
      continue;
    }
    bool seen = false;
    for (size_t k = 0; k < n && !seen; ++k) {
      seen = out[k].handle == h;
    }
    u64 id = 0;
    const bool have_id = R_SUCCEEDED(svcGetThreadId(&id, h));
    if (seen || (have_id && id == self_id)) {
      continue;
    }
    PausedThread& t = out[n++];
    t = PausedThread{};
    t.handle = h;
    t.thread_id = have_id ? id : 0;
    t.pause_rc = svcSetThreadActivity(h, ThreadActivity_Paused);
    ThreadContext ctx{};
    t.context_rc = svcGetThreadContext3(&ctx, h);
    if (R_SUCCEEDED(t.context_rc)) {
      t.pc = ctx.pc.x;
    }
  }
  return n;
}

void ResumeThreads(const PausedThread* threads, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (R_SUCCEEDED(threads[i].pause_rc)) {
      svcSetThreadActivity(threads[i].handle, ThreadActivity_Runnable);
    }
  }
}

namespace {

Thread g_worker;
bool g_worker_started = false;
UEvent g_job_ready;
std::atomic<void (*)(void*)> g_job{nullptr};
std::atomic<void*> g_job_arg{nullptr};
std::atomic<bool> g_job_busy{false};

void WorkerMain(void*) {
  for (;;) {
    waitSingle(waiterForUEvent(&g_job_ready), UINT64_MAX);
    if (auto* job = g_job.exchange(nullptr)) {
      job(g_job_arg.load());
      g_job_busy.store(false, std::memory_order_release);
    }
  }
}

}  // namespace

bool StartExitWorker() {
  if (g_worker_started) {
    return true;
  }
  ueventCreate(&g_job_ready, true);
  if (R_FAILED(threadCreate(&g_worker, WorkerMain, nullptr, nullptr, 128 * 1024, 0x2C, -2))) {
    return false;
  }
  if (R_FAILED(threadStart(&g_worker))) {
    threadClose(&g_worker);
    return false;
  }
  g_worker_started = true;
  return true;
}

bool RunOnExitWorker(void (*job)(void*), void* arg, u64 timeout_ms) {
  if (!g_worker_started || g_job_busy.exchange(true)) {
    return false;
  }
  g_job_arg.store(arg);
  g_job.store(job);
  ueventSignal(&g_job_ready);
  const u64 start = armGetSystemTick();
  const u64 limit = armNsToTicks(timeout_ms * 1'000'000ull);
  while (g_job_busy.load(std::memory_order_acquire)) {
    if (armGetSystemTick() - start >= limit) {
      return false;
    }
    svcSleepThread(5'000'000);
  }
  return true;
}

namespace {
int g_step_fd = -1;
char g_step_line[256];

void Append(char*& w, const char* end, const char* text) {
  while (*text && w < end) *w++ = *text++;
}
}  // namespace

bool OpenStepLog(const char* path) {
  g_step_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  return g_step_fd >= 0;
}

const char* Step(char mode, int n, const char* what) {
  char* w = g_step_line;
  const char* end = g_step_line + sizeof(g_step_line) - 2;
  const char mode_text[2] = {mode, 0};
  char number[12];
  int k = 0;
  char digits[12];
  unsigned v = unsigned(n);
  do {
    digits[k++] = char('0' + v % 10);
    v /= 10;
  } while (v && k < 11);
  for (int i = 0; i < k; ++i) number[i] = digits[k - 1 - i];
  number[k] = 0;
  Append(w, end, "exit mode ");
  Append(w, end, mode_text);
  Append(w, end, ": step ");
  Append(w, end, number);
  Append(w, end, ": ");
  Append(w, end, what);
  *w++ = '\n';
  *w = 0;
  if (g_step_fd >= 0) {
    (void)write(g_step_fd, g_step_line, size_t(w - g_step_line));
    fsync(g_step_fd);
  }
  return g_step_line;
}

char ParseExitMode(const char* value) {
  return value && (value[0] == 'D' || value[0] == 'd') && !value[1] ? 'D' : 'A';
}

const char* ExitModeDescription(char mode) {
  return mode == 'D' ? "tell every thread to end, join (2 s), exit(0)"
                     : "exit(0), as a return from main, threads still alive";
}

}  // namespace theft4::nx_exit
