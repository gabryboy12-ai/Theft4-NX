#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "input/sdl/sdl_hotplug_policy.h"

namespace rex::input::sdl {

TEST_CASE("SDL gamepad inventory preserves live pads and replaces reconnected instances",
          "[input][sdl][hotplug]") {
  const std::array<uint64_t, 2> open = {11, 22};
  const std::array<uint64_t, 2> connected = {22, 33};

  const GamepadInventoryPlan plan = PlanGamepadInventory(open, connected);

  CHECK(plan.remove == std::vector<uint64_t>{11});
  CHECK(plan.add == std::vector<uint64_t>{33});
}

TEST_CASE("SDL gamepad inventory ignores duplicate and invalid instance IDs",
          "[input][sdl][hotplug]") {
  const std::array<uint64_t, 1> open = {44};
  const std::array<uint64_t, 4> connected = {0, 44, 55, 55};

  const GamepadInventoryPlan plan = PlanGamepadInventory(open, connected);

  CHECK(plan.remove.empty());
  CHECK(plan.add == std::vector<uint64_t>{55});
}

}  // namespace rex::input::sdl
