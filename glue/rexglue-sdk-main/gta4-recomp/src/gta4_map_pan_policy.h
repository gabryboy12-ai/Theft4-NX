#pragma once

#include <cstdint>

namespace gta4::input {

struct MapPanAction {
  int32_t horizontal = 0;
  int32_t vertical = 0;
};

// Mouse, trackpad, and touchscreen map drags are direct manipulation: the
// map content follows the pointer. GTA's independent centered MapX / MapY
// action records already use that direction, so preserve the pointer delta
// here. Camera/look input is directional rather than direct manipulation and
// intentionally does not pass through this policy.
[[nodiscard]] constexpr MapPanAction DirectManipulationMapPan(int32_t pointer_delta_x,
                                                              int32_t pointer_delta_y) noexcept {
  return {.horizontal = pointer_delta_x, .vertical = pointer_delta_y};
}

}  // namespace gta4::input
