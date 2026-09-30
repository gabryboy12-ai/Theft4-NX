/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_POSIX, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#if REX_PLATFORM_NX
extern "C" {
#include <switch/arm/counter.h>
}
#endif

namespace rex::chrono {

// Switch: the ARM system counter (19.2 MHz), read directly. libnx's
// clock_gettime/clock_getres accept only CLOCK_REALTIME and CLOCK_MONOTONIC:
// with CLOCK_MONOTONIC_RAW they fail with EINVAL and leave the timespec
// untouched, the asserts below are off in Release, and the host tick count
// never moved. The guest time base stayed at 0 and run 12 spun forever in the
// game's `while (mftb == 0)` (sub_829E9CC8, docs/switch-port/06 section 15).
uint64_t Clock::host_tick_frequency_platform() {
#if REX_PLATFORM_NX
  return armGetSystemTickFreq();
#elif defined(__APPLE__)
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#else
  timespec res;
  int error = clock_getres(CLOCK_MONOTONIC_RAW, &res);
  assert_zero(error);
  assert_zero(res.tv_sec);  // Sub second resolution is required.

  // Convert nano seconds to hertz. Resolution is 1ns on most systems.
  return 1000000000ull / res.tv_nsec;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#if REX_PLATFORM_NX
  return armGetSystemTick();
#elif defined(__APPLE__)
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(CLOCK_MONOTONIC_RAW) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#else
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
