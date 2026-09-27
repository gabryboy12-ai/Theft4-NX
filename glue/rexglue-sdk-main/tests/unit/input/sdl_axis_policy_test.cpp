#include <array>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "input/sdl/sdl_axis_policy.h"

namespace rex::input::sdl {

TEST_CASE("An idle SDL thumbstick remains centered in XInput",
          "[input][sdl][axis]") {
  CHECK(ToXInputThumbY(0) == 0);
  CHECK(ToXInputThumbY(-1) == 1);
  CHECK(ToXInputThumbY(1) == -1);
}

TEST_CASE("SDL thumb Y reverses direction without overflowing at full travel",
          "[input][sdl][axis]") {
  const std::array<std::pair<int16_t, int16_t>, 5> positions{{
      {-32768, 32767},
      {-32767, 32767},
      {-16000, 16000},
      {16000, -16000},
      {32767, -32767},
  }};
  for (const auto [sdl_y, xinput_y] : positions) {
    CAPTURE(sdl_y);
    CHECK(ToXInputThumbY(sdl_y) == xinput_y);
  }
}

}  // namespace rex::input::sdl
