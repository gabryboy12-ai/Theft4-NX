// switch-game: stack sampling of the busy threads (nx_profiler.h). The
// sampling loop follows sa2-nx's sdk/src/ui/switch_perf.cpp (BSD-3-Clause,
// Copyright (c) 2026 Tom Clay; portions derived from Xenia, Copyright (c)
// 2022 Ben Vanik and Xenia project contributors), reduced to the samples and
// a per-thread histogram.

#include "nx_profiler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <switch.h>

namespace theft4::nx_profiler {

namespace {

constexpr size_t kMaxBusy = 32;
constexpr size_t kMaxSamples = 1 << 15;  // > 10 s at 1 kHz
constexpr int kFrames = 4;               // lr, then the x29 chain
constexpr uint64_t kSampleIntervalNs = 1'000'000ull;
constexpr uint64_t kReportNs = 10'000'000'000ull;
constexpr uint64_t kCopyNs = 100'000'000ull;
constexpr size_t kTopPcs = 14;
constexpr size_t kTopCallers = 6;

struct Sample {
  uint64_t pc;
  uint64_t frames[kFrames];
  uint32_t handle;
};

LogFn g_log = nullptr;
uint64_t g_base = 0, g_text_size = 0;
std::atomic<bool> g_running{false};
Thread g_thread;

// Written by the UI thread, copied by the profiler thread outside any pause.
std::mutex g_busy_mutex;
BusyThread g_busy[kMaxBusy];
size_t g_busy_count = 0;

// Only the profiler thread touches these.
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;
uint64_t g_paused_failures = 0;

std::string Offset(uint64_t address) {
  char text[24];
  if (address >= g_base && address < g_base + g_text_size) {
    std::snprintf(text, sizeof(text), "+0x%" PRIx64, address - g_base);
  } else {
    std::snprintf(text, sizeof(text), "abs0x%" PRIx64, address);
  }
  return text;
}

void AppendTop(std::string* out, std::unordered_map<uint64_t, uint32_t>& counts, size_t top) {
  std::vector<std::pair<uint64_t, uint32_t>> sorted(counts.begin(), counts.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t i = 0; i < sorted.size() && i < top; ++i) {
    char item[48];
    std::snprintf(item, sizeof(item), " %s x%u", Offset(sorted[i].first).c_str(),
                  sorted[i].second);
    *out += item;
  }
}

// Outside any pause: allocates and logs.
void Report(const BusyThread* busy, size_t busy_count, double seconds) {
  std::unordered_map<uint32_t, std::vector<const Sample*>> by_thread;
  for (size_t i = 0; i < g_sample_count; ++i) {
    by_thread[g_samples[i].handle].push_back(&g_samples[i]);
  }
  char head[192];
  std::snprintf(head, sizeof(head),
                "profile %.0f s: %zu samples of %zu threads (1 ms, in turn), %" PRIu64
                " pauses refused; tools/switch-game/where.py --profile adds them up",
                seconds, g_sample_count, by_thread.size(), g_paused_failures);
  g_log(head);
  std::vector<std::pair<uint32_t, size_t>> order;
  for (const auto& [handle, samples] : by_thread) {
    order.emplace_back(handle, samples.size());
  }
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [handle, n] : order) {
    const BusyThread* who = nullptr;
    for (size_t i = 0; i < busy_count; ++i) {
      if (busy[i].handle == handle) who = &busy[i];
    }
    std::unordered_map<uint64_t, uint32_t> pcs, callers;
    for (const Sample* s : by_thread[handle]) {
      ++pcs[s->pc];
      if (s->frames[0]) ++callers[s->frames[0]];
    }
    char line_head[160];
    std::snprintf(line_head, sizeof(line_head), "profile id %u %s '%s' (%.0f%% cpu) %zu samples:",
                  who ? who->id : 0, who ? who->kind : "?", who ? who->name : "?",
                  who ? who->percent : 0.0, n);
    std::string line = line_head;
    AppendTop(&line, pcs, kTopPcs);
    line += "; lr:";
    AppendTop(&line, callers, kTopCallers);
    g_log(line.c_str());
  }
  g_sample_count = 0;
  g_paused_failures = 0;
}

void TakeSample(uint32_t handle) {
  if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) {
    ++g_paused_failures;  // already paused by someone else: left alone
    return;
  }
  ThreadContext ctx;
  if (R_SUCCEEDED(svcGetThreadContext3(&ctx, handle)) && g_sample_count < kMaxSamples) {
    Sample& out = g_samples[g_sample_count++];
    out = {};
    out.handle = handle;
    out.pc = ctx.pc.x;
    out.frames[0] = ctx.lr;
    // The paused thread's own stack, only inside the region of its sp.
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp)) && (mi.perm & Perm_R)) {
      const uint64_t lo = mi.addr, hi = mi.addr + mi.size;
      uint64_t fp = ctx.fp;
      for (int k = 1; k < kFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
        out.frames[k] = reinterpret_cast<const uint64_t*>(fp)[1];
        const uint64_t next = reinterpret_cast<const uint64_t*>(fp)[0];
        if (next <= fp) break;
        fp = next;
      }
    }
  }
  svcSetThreadActivity(handle, ThreadActivity_Runnable);
}

void ProfilerMain(void*) {
  BusyThread local[kMaxBusy];
  size_t local_count = 0, next = 0;
  uint64_t last_copy = 0;
  uint64_t report_start = armGetSystemTick();
  for (;;) {
    svcSleepThread(kSampleIntervalNs);
    const uint64_t now = armGetSystemTick();
    if (armTicksToNs(now - last_copy) >= kCopyNs) {
      std::lock_guard<std::mutex> lock(g_busy_mutex);
      local_count = g_busy_count;
      std::memcpy(local, g_busy, sizeof(BusyThread) * local_count);
      last_copy = now;
    }
    if (local_count) {
      TakeSample(local[next++ % local_count].handle);
    }
    if (armTicksToNs(now - report_start) >= kReportNs) {
      Report(local, local_count, double(armTicksToNs(now - report_start)) / 1e9);
      report_start = armGetSystemTick();
    }
  }
}

}  // namespace

void Start(uint64_t module_base, uint64_t module_text_size, LogFn log) {
  g_base = module_base;
  g_text_size = module_text_size;
  g_log = log;
  // 0x2A: above the game's threads, so the samples are taken on time; it
  // sleeps almost all the time (sa2-nx uses the same).
  if (R_SUCCEEDED(threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A, -2)) &&
      R_SUCCEEDED(threadStart(&g_thread))) {
    g_running = true;
  }
}

bool Running() { return g_running.load(std::memory_order_relaxed); }

void SetBusyThreads(const BusyThread* threads, size_t count) {
  std::lock_guard<std::mutex> lock(g_busy_mutex);
  g_busy_count = std::min(count, kMaxBusy);
  std::memcpy(g_busy, threads, sizeof(BusyThread) * g_busy_count);
}

}  // namespace theft4::nx_profiler
