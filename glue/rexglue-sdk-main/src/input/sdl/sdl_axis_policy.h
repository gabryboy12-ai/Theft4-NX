#pragma once

#include <cstdint>
#include <limits>

namespace rex::input::sdl {

// SDL thumb Y grows downward; XInput grows upward. Both define zero as
// centered. Bitwise complement shifts centered input to -1, so invert the
// signed magnitude and saturate the asymmetric negative endpoint instead.
constexpr int16_t ToXInputThumbY(int16_t axis) {
  return axis == std::numeric_limits<int16_t>::min()
             ? std::numeric_limits<int16_t>::max()
             : static_cast<int16_t>(-axis);
}

}  // namespace rex::input::sdl
