#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rex::graphics::nx_guest_gpu_progress {

// Guest predicates and their watchdog stay in generated code. This gate only
// stops repeated host execution of an unchanged GPU progress poll. A timeout
// bounds observation of title error flags and writes by non-GPU guest threads,
// whose stores do not publish this host event.
class Gate {
 public:
  void SetEnabled(bool value) {
    std::lock_guard lock(mutex_);
    enabled_.store(value, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    changed_.notify_all();
  }

  bool enabled() const { return enabled_.load(std::memory_order_acquire); }

  uint64_t Snapshot() const {
    return generation_.load(std::memory_order_acquire);
  }

  // Publish only after the existing guest-visible stores. Sharing mutex_ with
  // Wait closes the check-to-sleep race, including notification before Wait.
  void Signal() {
    if (!enabled()) return;
    std::lock_guard lock(mutex_);
    generation_.fetch_add(1, std::memory_order_release);
    signals_.fetch_add(1, std::memory_order_relaxed);
    changed_.notify_all();
  }

  void Wait(uint64_t observed_generation) {
    std::unique_lock lock(mutex_);
    if (!enabled() || Snapshot() != observed_generation) return;
    waits_.fetch_add(1, std::memory_order_relaxed);
    const bool signaled = changed_.wait_for(
        lock, std::chrono::milliseconds(1), [&] {
          return !enabled() || Snapshot() != observed_generation;
        });
    (signaled ? wakes_ : timeouts_).fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t signals() const { return signals_.load(std::memory_order_relaxed); }
  uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }
  uint64_t wakes() const { return wakes_.load(std::memory_order_relaxed); }
  uint64_t timeouts() const { return timeouts_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> generation_{0};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<uint64_t> signals_{0}, waits_{0}, wakes_{0}, timeouts_{0};
};

inline Gate& GlobalGate() {
  static Gate gate;
  return gate;
}

// The initial timebase stamp and type distinguish reused stack monitors. The
// first 64 unchanged polls stay active for short GPU operations; progress or a
// different wait always restarts that budget. No host state alters guest data.
class PollState {
 public:
  bool ShouldWait(uint32_t monitor, uint32_t device, uint32_t started,
                  uint32_t type, uint32_t progress, uint64_t generation) {
    if (monitor != monitor_ || device != device_ || started != started_ ||
        type != type_ || progress != progress_ || generation != generation_) {
      monitor_ = monitor;
      device_ = device;
      started_ = started;
      type_ = type;
      progress_ = progress;
      generation_ = generation;
      polls_ = 0;
      return false;
    }
    if (polls_ < 64) {
      ++polls_;
      return false;
    }
    return true;
  }

 private:
  uint32_t monitor_ = 0, device_ = 0, started_ = 0, type_ = 0, progress_ = 0;
  uint64_t generation_ = ~uint64_t(0);
  uint32_t polls_ = 0;
};

inline void Hint(uint32_t monitor, uint32_t device, uint32_t started,
                 uint32_t type, uint32_t progress, uint64_t observed_generation) {
  auto& gate = GlobalGate();
  if (!gate.enabled()) return;
  thread_local PollState state;
  if (state.ShouldWait(monitor, device, started, type, progress,
                       observed_generation)) {
    gate.Wait(observed_generation);
  }
}

}  // namespace rex::graphics::nx_guest_gpu_progress
