#include <catch2/catch_test_macros.hpp>

#include "input/sdl/sdl_rumble_policy.h"

namespace rex::input::sdl {

TEST_CASE("SDL rumble result follows SDL3 boolean semantics", "[input][sdl][rumble]") {
  CHECK(TranslateSdlRumbleResult(true) == X_ERROR_SUCCESS);
  CHECK(TranslateSdlRumbleResult(false) == X_ERROR_FUNCTION_FAILED);
}

TEST_CASE("SDL rumble duration represents persistent XInput state", "[input][sdl][rumble]") {
  CHECK(GetRumbleDurationMs(0, 0) == 0);
  CHECK(GetRumbleDurationMs(1, 0) == 65535);
  CHECK(GetRumbleDurationMs(0, 1) == 65535);
  CHECK(GetRumbleDurationMs(65535, 65535) == 65535);
}

TEST_CASE("SDL rumble refresh occurs only at its scheduled deadline", "[input][sdl][rumble]") {
  CHECK_FALSE(ShouldRefreshRumble(0, 0));
  CHECK_FALSE(ShouldRefreshRumble(32766, 32767));
  CHECK(ShouldRefreshRumble(32767, 32767));
  CHECK(ShouldRefreshRumble(32768, 32767));
}

}  // namespace rex::input::sdl
