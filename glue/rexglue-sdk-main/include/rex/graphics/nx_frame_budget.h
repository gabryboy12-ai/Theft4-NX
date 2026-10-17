#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
namespace rex::graphics::nx_frame_budget {
constexpr bool SplitFsiSampleRatePass(uint32_t previous, uint32_t next, bool variable_rate) {
  return previous != next && !variable_rate;
}
inline std::atomic<uint64_t>& AvoidedPassSplits() {
  static std::atomic<uint64_t> count{0};
  return count;
}
struct Sample { uint64_t intervals=0, over_budget=0, maximum_ns=0; };
class Tracker {
 public:
  // One successful guest-output publication per record. This measures guest
  // output cadence, not display scanout latency or GPU execution duration.
  void Record(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_ns_ && now_ns >= last_ns_) {
      const uint64_t interval = now_ns-last_ns_;
      ++sample_.intervals;
      sample_.over_budget += interval > UINT64_C(33333334);
      if (interval > sample_.maximum_ns) sample_.maximum_ns=interval;
    }
    last_ns_=now_ns;
  }
  Sample Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Sample result=sample_;sample_={};return result;
  }
 private:
  std::mutex mutex_;
  uint64_t last_ns_=0;
  Sample sample_;
};
inline Tracker& GuestOutput() { static Tracker tracker;return tracker; }
}
