#pragma once

// Runtime switches for the Switch synchronisation fast paths (C2). They are
// plain relaxed atomics so the A/B schedule can flip them while the game runs;
// every path they select is correct on its own, they only differ in cost.

#include <atomic>

namespace rex::thread::nx_sync {

// Object table and dispatcher-header lookups without the global critical
// region (lookups only; every table update still takes the lock). Objects are
// freed after a grace period so a concurrent lookup never touches freed memory.
inline std::atomic<bool>& FastObjectLookup() {
  static std::atomic<bool> value{true};
  return value;
}

// Wake one waiter instead of all of them when only one can proceed: auto-reset
// events and timers, mutants, and semaphore releases of one count.
inline std::atomic<bool>& NotifyOne() {
  static std::atomic<bool> value{true};
  return value;
}

// Guest mfmsr/mtmsrd interrupt lock tracked per thread instead of on the
// global critical region (system/ppc_generated.cpp).
inline std::atomic<bool>& GuestInterruptLockFree() {
  static std::atomic<bool> value{true};
  return value;
}

// C5: semaphore releases and event signals without taking the object mutex
// unless a thread may be asleep on it, and wake-ups sent after the mutex is
// released (rex/thread/nx_multi_wait.h).
inline std::atomic<bool>& LockFreeRelease() {
  static std::atomic<bool> value{true};
  return value;
}

}  // namespace rex::thread::nx_sync
