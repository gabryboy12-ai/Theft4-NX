// Periodic UI-thread measurements. No work is added to the GPU draw path.
#pragma once
#include <cerrno>
#include <cstddef>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
namespace theft4 {
struct PerfSample {
  double elapsed_s, window_s;
  uint64_t frames, draws, submits;
  double fence_wait_ms, command_cpu_ms, submit_ms, present_ms;
  uint64_t pipelines;
  double pipeline_ms;
  uint64_t shaders;
  double shader_ms;
  uint64_t submit_failures, present_failures;
};
class PerfCsv {
 public:
  ~PerfCsv() { if (file_) std::fclose(file_); }
  PerfCsv() = default;
  PerfCsv(const PerfCsv&) = delete;
  PerfCsv& operator=(const PerfCsv&) = delete;
  bool Open(const char* directory) {
    if (file_) return true;
    for (unsigned i = 1; i <= 9999; ++i) {
      int n = std::snprintf(path_, sizeof(path_), "%s/perf-%04u.csv", directory, i);
      if (n < 0 || std::size_t(n) >= sizeof(path_)) return false;
      int fd = ::open(path_, O_WRONLY | O_CREAT | O_EXCL, 0666);
      if (fd < 0) { if (errno == EEXIST) continue; return false; }
      file_ = ::fdopen(fd, "w");
      if (!file_) { ::close(fd); return false; }
      return std::fputs("elapsed_s,window_s,frames,draws,submits,fence_wait_ms,command_cpu_ms,submit_ms,present_ms,pipelines,pipeline_ms,shaders,shader_ms,submit_failures,present_failures\n", file_) >= 0 && std::fflush(file_) == 0;
    }
    return false;
  }
  const char* path() const { return path_; }
  bool Write(const PerfSample& s) {
    if (!file_ || !std::isfinite(s.elapsed_s) || !std::isfinite(s.window_s) || s.window_s <= 0) return false;
    const int n = std::fprintf(file_, "%.6f,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f,%" PRIu64 ",%.6f,%" PRIu64 ",%.6f,%" PRIu64 ",%" PRIu64 "\n",
      s.elapsed_s, s.window_s, s.frames, s.draws, s.submits, s.fence_wait_ms, s.command_cpu_ms,
      s.submit_ms, s.present_ms, s.pipelines, s.pipeline_ms, s.shaders, s.shader_ms,
      s.submit_failures, s.present_failures);
    // Buffered filesystem writes only; no fsync per heartbeat.
    return n > 0 && std::fflush(file_) == 0;
  }
 private:
  std::FILE* file_ = nullptr;
  char path_[512]{};
};
}  // namespace theft4
