// Switch condition waits. Each multi-object wait subscribes only to the
// conditions it actually waits on; there is no process-wide wakeup event.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/thread/nx_sync_tuning.h>

namespace rex::thread::detail {

class NxMultiWaiter {
 public:
  uint64_t sequence() {
    std::lock_guard lock(mutex_);
    return sequence_;
  }

  void Notify() {
    std::lock_guard lock(mutex_);
    ++sequence_;
    condition_.notify_one();
  }

  void WaitForChange(uint64_t previous,
                     std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    auto changed = [&] { return sequence_ != previous; };
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      condition_.wait(lock, changed);
    } else {
      condition_.wait_until(lock, deadline, changed);
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  uint64_t sequence_ = 0;
};

// Result is the runtime's WaitResult. Keeping the implementation independent
// of platform declarations also permits concurrency tests of the actual code.
template <typename Result>
class NxConditionBase {
 public:
  virtual ~NxConditionBase() = default;
  virtual bool Signal() = 0;

  Result Wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    // Registered before the predicate is read: a lock-free signaller that
    // updates the state and then sees no sleepers is ordered before this read
    // (both sides are sequentially consistent), so it cannot be missed.
    sleepers_.fetch_add(1, std::memory_order_seq_cst);
    auto predicate = [this] { return signaled(); };
    bool executed = predicate();
    if (!executed) {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    sleepers_.fetch_sub(1, std::memory_order_seq_cst);
    if (executed) {
      post_execution();
      if (nx_sync::LockFreeRelease().load(std::memory_order_relaxed)) {
        // Pass a remaining signal on after unlocking, so the woken thread does
        // not immediately block on the mutex this thread still holds.
        const bool pass_on = signaled();
        lock.unlock();
        if (pass_on) NotifyCondition(true);
      } else {
        PassOnLocked();
      }
      return Result::kSuccess;
    }
    return Result::kTimeout;
  }

  static std::pair<Result, size_t> WaitMultiple(
      std::vector<NxConditionBase*>&& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    if (handles.empty()) return {Result::kFailed, 0};
    if (handles.size() == 1) return {handles[0]->Wait(timeout), 0};

    // A WaitAny may name an object more than once: retain caller order for
    // result selection, but lock and subscribe to each condition only once.
    // WaitAll requires distinct objects, and fails without consuming any of
    // them when a duplicate is supplied.
    std::vector<NxConditionBase*> unique_handles;
    unique_handles.reserve(handles.size());
    for (auto* handle : handles) {
      if (std::find(unique_handles.begin(), unique_handles.end(), handle) !=
          unique_handles.end()) {
        if (wait_all) return {Result::kFailed, 0};
      } else {
        unique_handles.push_back(handle);
      }
    }
    if (unique_handles.size() == 1) return {unique_handles[0]->Wait(timeout), 0};

    const auto deadline = timeout == std::chrono::milliseconds::max()
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + timeout;
    NxMultiWaiter waiter;
    // Declared after waiter: removes all subscriptions before waiter dies.
    Registration registration(waiter, unique_handles.size());
    for (auto* handle : unique_handles) registration.Add(*handle);
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(unique_handles.size());

    for (;;) {
      // Capture before checking object predicates. A signal arriving between
      // that check and sleeping advances this sequence, so it cannot be lost.
      const uint64_t previous = waiter.sequence();
      bool all_locked = true;
      for (auto* handle : unique_handles) {
        locks.emplace_back(handle->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
      }
      if (all_locked) {
        size_t first = std::numeric_limits<size_t>::max();
        bool met = wait_all;
        for (size_t i = 0; i < handles.size(); ++i) {
          const bool ready = handles[i]->signaled();
          if (ready && first == std::numeric_limits<size_t>::max()) first = i;
          if (wait_all && !ready) {
            met = false;
            break;
          }
          if (!wait_all && ready) {
            met = true;
            break;
          }
        }
        if (met) {
          if (wait_all) {
            for (auto* handle : handles) handle->post_execution();
            for (auto* handle : unique_handles) handle->PassOnLocked();
          } else {
            handles[first]->post_execution();
            handles[first]->PassOnLocked();
          }
          // Clear before Registration's destructor acquires these mutexes.
          locks.clear();
          return {Result::kSuccess, first};
        }
      }
      locks.clear();
      if (std::chrono::steady_clock::now() >= deadline) {
        return {Result::kTimeout, 0};
      }
      if (!all_locked) {
        // Lock contention is not a signal transition. Do not park forever
        // expecting a notification merely because an object mutex was busy.
        std::this_thread::yield();
        continue;
      }
      waiter.WaitForChange(previous, deadline);
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  virtual bool signaled() const = 0;
  virtual void post_execution() = 0;

  // Caller holds mutex_. Notify only subscribers to this object. Taking the
  // waiter's mutex here is safe: a waiter never acquires an object mutex while
  // holding its own mutex. Registration removal uses this same object lock.
  //
  // single_winner: at most one single-object waiter can consume this signal
  // (auto-reset event or timer, mutant, one semaphore count). Waking all of
  // them makes every waiter but one go back to sleep after taking the mutex:
  // on GTA IV's job threads that herd was most of their CPU (C1 profile).
  // Multi-object waiters are always notified; they re-check every object.
  void NotifySignaledLocked(bool single_winner = false) {
    if (single_winner && nx_sync::NotifyOne().load(std::memory_order_relaxed)) {
      cond_.notify_one();
    } else {
      cond_.notify_all();
    }
    for (auto* waiter : multi_waiters_) waiter->Notify();
  }

  // C5: for a signaller that already updated the state with an atomic (no
  // mutex held). Takes the mutex only if a thread may be waiting, and only to
  // notify multi-object waiters; the condition variable is notified after the
  // mutex is released (legal, and no lost wake-up: a single-object waiter
  // holds the mutex from registering in sleepers_ until it sleeps, so the
  // lock/unlock here orders this notify after it went to sleep).
  void WakeSleepers(bool single_winner) {
    if (sleepers_.load(std::memory_order_seq_cst) == 0) return;
    {
      std::lock_guard lock(mutex_);
      for (auto* waiter : multi_waiters_) waiter->Notify();
    }
    NotifyCondition(single_winner);
  }

  void NotifyCondition(bool single_winner) {
    if (single_winner && nx_sync::NotifyOne().load(std::memory_order_relaxed)) {
      cond_.notify_one();
    } else {
      cond_.notify_all();
    }
  }

  // Single-object waiters inside Wait() plus multi-object registrations.
  std::atomic<uint32_t> sleepers_{0};

  // Caller holds mutex_: after a single-object waiter consumed the signal,
  // pass it on if the object is still signalled (semaphore with counts left,
  // manual-reset event), so a notify_one never strands a signal.
  void PassOnLocked() {
    if (signaled()) {
      if (nx_sync::NotifyOne().load(std::memory_order_relaxed)) {
        cond_.notify_one();
      } else {
        cond_.notify_all();
      }
    }
  }

  std::condition_variable cond_;
  std::mutex mutex_;

 private:
  class Registration {
   public:
    Registration(NxMultiWaiter& waiter, size_t capacity) : waiter_(waiter) {
      handles_.reserve(capacity);
    }
    void Add(NxConditionBase& handle) {
      std::lock_guard lock(handle.mutex_);
      // Track first so an allocation failure in the subscriber list is safe.
      handles_.push_back(&handle);
      handle.multi_waiters_.push_back(&waiter_);
      handle.sleepers_.fetch_add(1, std::memory_order_seq_cst);
    }
    ~Registration() {
      for (auto* handle : handles_) {
        std::lock_guard lock(handle->mutex_);
        auto& list = handle->multi_waiters_;
        auto found = std::find(list.begin(), list.end(), &waiter_);
        if (found != list.end()) {
          list.erase(found);
          handle->sleepers_.fetch_sub(1, std::memory_order_seq_cst);
        }
      }
    }
   private:
    NxMultiWaiter& waiter_;
    std::vector<NxConditionBase*> handles_;
  };
  std::vector<NxMultiWaiter*> multi_waiters_;
};

}  // namespace rex::thread::detail
