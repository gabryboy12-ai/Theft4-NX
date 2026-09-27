#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace rex::input::sdl {

struct GamepadInventoryPlan {
  std::vector<uint64_t> remove;
  std::vector<uint64_t> add;
};

inline GamepadInventoryPlan PlanGamepadInventory(std::span<const uint64_t> open_ids,
                                                 std::span<const uint64_t> connected_ids) {
  GamepadInventoryPlan plan;

  const auto contains = [](std::span<const uint64_t> ids, uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  const auto append_unique = [](std::vector<uint64_t>& ids, uint64_t id) {
    if (id && std::find(ids.begin(), ids.end(), id) == ids.end()) {
      ids.push_back(id);
    }
  };

  for (const uint64_t id : open_ids) {
    if (id && !contains(connected_ids, id)) {
      append_unique(plan.remove, id);
    }
  }
  for (const uint64_t id : connected_ids) {
    if (id && !contains(open_ids, id)) {
      append_unique(plan.add, id);
    }
  }
  return plan;
}

}  // namespace rex::input::sdl
