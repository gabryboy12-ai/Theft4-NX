// switch-smoke step 6b: the SD card's own ceiling (run 19, 06 section 25).
//
// Before blaming the runtime for slow loading: what the SD gives through
// newlib/fsdev, the path the game's reads take (open/lseek/read), on a large
// game file. Four read tests of 10 s each: sequential 1 MiB blocks and random
// 64 KiB blocks, with 1 and with 3 threads (3 threads: one descriptor each,
// sequential ones each on its own third of the file). Then 5 s of 32 KiB
// appends to a scratch file, the pattern of the game's cache: copy (run 18:
// 14 ms per 32 KiB write), deleted afterwards. Each test: MB/s, operations,
// median and largest latency.

#include "sd_bench.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "probes.h"

namespace {

constexpr const char* kScratchPath = "sdmc:/switch/theft4/sdbench.tmp";

struct Worker {
  // In
  const char* path = nullptr;
  bool random = false;
  size_t block = 0;
  u64 region_start = 0, region_size = 0;
  u64 duration_ns = 0;
  u32 seed = 1;
  // Out
  std::vector<u32> latency_us;
  u64 bytes = 0;
  int error = 0;  // errno of the first failure
  Thread thread{};
};

void WorkerMain(void* arg) {
  auto* w = static_cast<Worker*>(arg);
  int fd = open(w->path, O_RDONLY);
  if (fd < 0) {
    w->error = errno;
    return;
  }
  void* buffer = aligned_alloc(0x1000, w->block);
  if (!buffer) {
    w->error = ENOMEM;
    close(fd);
    return;
  }
  const u64 blocks = w->region_size / w->block;
  u64 next = 0;
  u32 state = w->seed;
  const u64 start = armGetSystemTick();
  while (armTicksToNs(armGetSystemTick() - start) < w->duration_ns && blocks) {
    u64 index;
    if (w->random) {
      state = state * 1664525u + 1013904223u;  // LCG, enough to scatter
      index = (u64(state) << 16 ^ armGetSystemTick()) % blocks;
    } else {
      index = next++ % blocks;
    }
    const u64 t0 = armGetSystemTick();
    if (lseek(fd, off_t(w->region_start + index * w->block), SEEK_SET) < 0) {
      w->error = errno;
      break;
    }
    const ssize_t got = read(fd, buffer, w->block);
    const u64 t1 = armGetSystemTick();
    if (got <= 0) {
      w->error = got < 0 ? errno : -1;
      break;
    }
    w->bytes += u64(got);
    w->latency_us.push_back(u32(armTicksToNs(t1 - t0) / 1000));
  }
  free(buffer);
  close(fd);
}

void Summarize(const char* name, std::vector<Worker>& workers, u64 elapsed_ns) {
  std::vector<u32> all;
  u64 bytes = 0;
  int error = 0;
  for (auto& w : workers) {
    all.insert(all.end(), w.latency_us.begin(), w.latency_us.end());
    bytes += w.bytes;
    if (!error) error = w.error;
  }
  if (all.empty()) {
    SMOKE_INFO("  %s: no operation (errno %d)", name, error);
    return;
  }
  std::sort(all.begin(), all.end());
  const double seconds = double(elapsed_ns) / 1e9;
  SMOKE_INFO("  %s: %.1f MB/s, %zu ops, latency median %.2f ms, p95 %.2f ms, max %.2f ms%s",
             name, double(bytes) / 1e6 / seconds, all.size(), all[all.size() / 2] / 1000.0,
             all[all.size() * 95 / 100] / 1000.0, all.back() / 1000.0,
             error ? " (stopped on an error)" : "");
}

void RunReadTest(const char* name, const char* path, u64 file_size, bool random, size_t block,
                 int threads, u64 duration_ns) {
  std::vector<Worker> workers(threads);
  for (int i = 0; i < threads; ++i) {
    Worker& w = workers[i];
    w.path = path;
    w.random = random;
    w.block = block;
    w.duration_ns = duration_ns;
    w.seed = 0x9E3779B9u * u32(i + 1);
    w.latency_us.reserve(random ? 200000 : 20000);
    if (random) {
      w.region_start = 0;
      w.region_size = file_size;
    } else {
      const u64 part = (file_size / u64(threads)) & ~u64(block - 1);
      w.region_start = part * u64(i);
      w.region_size = part;
    }
  }
  const u64 start = armGetSystemTick();
  int started = 0;
  for (auto& w : workers) {
    // Priority 0x2C (the runtime's host band), any core.
    if (R_SUCCEEDED(threadCreate(&w.thread, WorkerMain, &w, nullptr, 0x8000, 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&w.thread))) {
      ++started;
    } else {
      w.error = -2;
      w.thread.handle = INVALID_HANDLE;
    }
  }
  for (auto& w : workers) {
    if (w.thread.handle != INVALID_HANDLE) {
      threadWaitForExit(&w.thread);
      threadClose(&w.thread);
    }
  }
  const u64 elapsed = armTicksToNs(armGetSystemTick() - start);
  char label[96];
  std::snprintf(label, sizeof(label), "%s, %d thread%s", name, threads, threads > 1 ? "s" : "");
  if (started != threads) {
    SMOKE_INFO("  %s: only %d of %d threads started", label, started, threads);
  }
  Summarize(label, workers, elapsed);
}

// The game's cache: copy writes 32 KiB at a time to a growing file.
void RunAppendTest(u64 duration_ns) {
  int fd = open(kScratchPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    SMOKE_INFO("  append 32 KiB: cannot create %s (errno %d)", kScratchPath, errno);
    return;
  }
  std::vector<char> block(32768, 0x5A);
  std::vector<u32> latency;
  latency.reserve(100000);
  u64 bytes = 0;
  int error = 0;
  const u64 start = armGetSystemTick();
  while (armTicksToNs(armGetSystemTick() - start) < duration_ns && bytes < (256ull << 20)) {
    const u64 t0 = armGetSystemTick();
    const ssize_t put = write(fd, block.data(), block.size());
    const u64 t1 = armGetSystemTick();
    if (put <= 0) {
      error = errno;
      break;
    }
    bytes += u64(put);
    latency.push_back(u32(armTicksToNs(t1 - t0) / 1000));
  }
  const u64 elapsed = armTicksToNs(armGetSystemTick() - start);
  close(fd);
  unlink(kScratchPath);
  if (latency.empty()) {
    SMOKE_INFO("  append 32 KiB: no write (errno %d)", error);
    return;
  }
  std::sort(latency.begin(), latency.end());
  SMOKE_INFO("  append 32 KiB to a new file (as the cache: copy), 1 thread: %.1f MB/s, %zu ops, "
             "latency median %.2f ms, p95 %.2f ms, max %.2f ms%s",
             double(bytes) / 1e6 / (double(elapsed) / 1e9), latency.size(),
             latency[latency.size() / 2] / 1000.0, latency[latency.size() * 95 / 100] / 1000.0,
             latency.back() / 1000.0, error ? " (stopped on an error)" : "");
}

}  // namespace

void RunSdBenchmark(const char* path, int seconds) {
  struct stat st {};
  if (stat(path, &st) != 0 || st.st_size < (64 << 20)) {
    SMOKE_INFO("step 6b SD benchmark: %s missing or under 64 MiB (errno %d): skipped", path,
               errno);
    return;
  }
  const u64 size = u64(st.st_size);
  const u64 duration = u64(seconds) * 1'000'000'000ull;
  SMOKE_INFO("step 6b SD benchmark: %s (%" PRIu64 " MiB), %d s per read test, through "
             "newlib/fsdev (open, lseek, read) as the game's reads",
             path, size >> 20, seconds);
  RunReadTest("sequential 1 MiB", path, size, false, 1 << 20, 1, duration);
  RunReadTest("sequential 1 MiB", path, size, false, 1 << 20, 3, duration);
  RunReadTest("random 64 KiB", path, size, true, 64 << 10, 1, duration);
  RunReadTest("random 64 KiB", path, size, true, 64 << 10, 3, duration);
  RunAppendTest(5'000'000'000ull);
}
