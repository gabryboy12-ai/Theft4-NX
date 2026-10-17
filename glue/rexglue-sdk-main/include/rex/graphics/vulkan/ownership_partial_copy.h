#pragma once

#include <rex/graphics/vulkan/ownership_copy.h>

namespace rex::graphics::vulkan::ownership_copy {

inline bool Intersects(const Rectangle& a, const Rectangle& b) {
  return uint64_t(a.x) < uint64_t(b.x) + b.width &&
         uint64_t(b.x) < uint64_t(a.x) + a.width &&
         uint64_t(a.y) < uint64_t(b.y) + b.height &&
         uint64_t(b.y) < uint64_t(a.y) + a.height;
}

// Reordering one transfer ahead of the fallback draws is permitted only when
// its destination rectangles do not intersect any other transfer destination.
// Callers additionally reject destination-as-source and host-depth aliases.
inline bool DisjointFromOthers(
    const std::vector<std::vector<Rectangle>>& destinations, size_t selected) {
  if (selected >= destinations.size() || destinations[selected].empty()) return false;
  for (const auto& a : destinations[selected]) {
    if (!a.width || !a.height) return false;
    for (size_t i = 0; i < destinations.size(); ++i) {
      if (i == selected) continue;
      for (const auto& b : destinations[i]) {
        if (!b.width || !b.height) return false;
        if (Intersects(a, b)) return false;
      }
    }
  }
  return true;
}

// Distinct source images can be transitioned together before a copy batch only
// when its destination writes are independent. Include rectangles within each
// transfer too: vkCmdCopyImage does not order overlapping regions in one call.
inline bool PairwiseDisjoint(
    const std::vector<std::vector<Rectangle>>& destinations) {
  for (size_t i = 0; i < destinations.size(); ++i) {
    if (destinations[i].empty()) return false;
    for (size_t a = 0; a < destinations[i].size(); ++a) {
      const Rectangle& rectangle = destinations[i][a];
      if (!rectangle.width || !rectangle.height) return false;
      for (size_t j = i; j < destinations.size(); ++j) {
        for (size_t b = j == i ? a + 1 : 0; b < destinations[j].size(); ++b) {
          const Rectangle& other = destinations[j][b];
          if (!other.width || !other.height || Intersects(rectangle, other)) return false;
        }
      }
    }
  }
  return !destinations.empty();
}

}  // namespace rex::graphics::vulkan::ownership_copy
