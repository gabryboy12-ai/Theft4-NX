#pragma once

#include <cstdint>

#include <rex/input/input.h>
#include <rex/system/xtypes.h>

namespace rex::input::sdl {

// SDL models rumble as a finite-duration effect, while XInput models it as
// persistent controller state. Keep the SDL effect alive for its maximum
// supported duration and renew it halfway through that window while the guest
// continues requesting non-zero motors.
constexpr uint32_t kRumbleDurationMs = 65535;
constexpr uint32_t kRumbleRefreshIntervalMs = 32767;

inline uint32_t GetRumbleDurationMs(uint16_t left_motor, uint16_t right_motor) {
  return left_motor || right_motor ? kRumbleDurationMs : 0;
}

inline bool ShouldRefreshRumble(uint64_t now_ms, uint64_t next_refresh_ms) {
  return next_refresh_ms && now_ms >= next_refresh_ms;
}

inline X_RESULT TranslateSdlRumbleResult(bool succeeded) {
  return succeeded ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}

}  // namespace rex::input::sdl
