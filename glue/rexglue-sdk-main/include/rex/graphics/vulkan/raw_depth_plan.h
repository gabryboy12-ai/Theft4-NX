#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include <rex/graphics/vulkan/ownership_copy.h>

namespace rex::graphics::vulkan::raw_depth {
struct Bounds {
  uint32_t x = UINT32_MAX, y = UINT32_MAX, end_x = 0, end_y = 0;
  bool Include(uint32_t px, uint32_t py, uint32_t width, uint32_t height) {
    if (!width || !height || uint64_t(px) + width > INT32_MAX ||
        uint64_t(py) + height > INT32_MAX) return false;
    x = std::min(x, px); y = std::min(y, py);
    end_x = std::max(end_x, px + width); end_y = std::max(end_y, py + height);
    return true;
  }
  uint32_t width() const { return end_x > x ? end_x - x : 0; }
  uint32_t height() const { return end_y > y ? end_y - y : 0; }
  uint64_t pixels() const { return uint64_t(width()) * height(); }
};
// Match the old non-wrapping copy planner. Shader wrapping must never silently
// change an accepted copy from a padded tile beyond the 2048-tile address space.
inline bool UnwrappedTiles(const ownership_copy::Surface& source,
    const ownership_copy::Surface& destination, uint32_t tile_width,
    uint32_t tile_height, const ownership_copy::Rectangle& r) {
  if (!source.pitch_tiles || source.pitch_tiles > 2048 ||
      !destination.pitch_tiles || destination.pitch_tiles > 2048 ||
      source.base_tiles >= 2048 || destination.base_tiles >= 2048 ||
      source.width > INT32_MAX || source.height > INT32_MAX ||
      destination.width > INT32_MAX || destination.height > INT32_MAX ||
      !r.width || !r.height || !tile_width || !tile_height ||
      uint64_t(r.x) + r.width > destination.width ||
      uint64_t(r.y) + r.height > destination.height) return false;
  const int64_t delta = int64_t(destination.base_tiles) - source.base_tiles;
  const int64_t first = delta + int64_t(r.y / tile_height) * destination.pitch_tiles + r.x / tile_width;
  const int64_t last = delta + int64_t((r.y + r.height - 1) / tile_height) * destination.pitch_tiles +
                       (r.x + r.width - 1) / tile_width;
  return first >= 0 && last < 2048;
}
// Packed D32/S8 in the pinned NVK driver takes one depth copy and two stencil
// copies per source bounding box and destination rectangle. Account for both;
// use this route only for a substantial launch reduction and bounded extra data.
inline bool Worthwhile(uint64_t old_regions, uint64_t sources,
    uint64_t rectangles, uint64_t copied_pixels, uint64_t source_bbox_pixels,
    uint64_t destination_bbox_pixels) {
  if (!copied_pixels || !sources || !rectangles || old_regions <= 16 ||
      sources > 7 || rectangles > 64) return false;
  if (old_regions <= 2 * (3 * sources + 3 * rectangles)) return false;
  const uint64_t limit = copied_pixels > UINT64_MAX / 4 ? UINT64_MAX : copied_pixels * 4;
  if (!source_bbox_pixels || !destination_bbox_pixels ||
      source_bbox_pixels > limit || destination_bbox_pixels > limit) return false;
  return true;
}
}  // namespace rex::graphics::vulkan::raw_depth
