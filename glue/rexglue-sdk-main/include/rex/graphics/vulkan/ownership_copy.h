#pragma once

// ReXGlue runtime: tile-preserving render-target copy planning.
// Released under the BSD license; see LICENSE in the SDK root.

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

namespace rex::graphics::vulkan::ownership_copy {

// Describe a tile-preserving copy independently of Vulkan so the address
// mapping can be checked against the shader's per-pixel EDRAM addressing.
struct Surface {
  uint32_t base_tiles, pitch_tiles, width, height;
};
struct Rectangle {
  uint32_t x, y, width, height;
};
struct Region {
  uint32_t source_x, source_y, destination_x, destination_y, width, height;
};

// Joining only exact neighbors with the same source/destination translation
// preserves every pixel and never fills a cutout. Rectangle enumeration order
// is irrelevant, including when the original vertical scan met an expired run.
inline void CoalesceRegions(std::vector<Region>& regions) {
  size_t previous_count;
  do {
    previous_count = regions.size();
    std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
      return std::make_tuple(a.destination_y, a.source_y, a.height,
                             int64_t(a.source_x) - a.destination_x, a.destination_x) <
             std::make_tuple(b.destination_y, b.source_y, b.height,
                             int64_t(b.source_x) - b.destination_x, b.destination_x);
    });
    size_t count = 0;
    for (const Region& region : regions) {
      if (count) {
        Region& previous = regions[count - 1];
        if (previous.destination_y == region.destination_y &&
            previous.source_y == region.source_y && previous.height == region.height &&
            uint64_t(previous.destination_x) + previous.width == region.destination_x &&
            uint64_t(previous.source_x) + previous.width == region.source_x) {
          previous.width += region.width;
          continue;
        }
      }
      regions[count++] = region;
    }
    regions.resize(count);
    std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
      return std::make_tuple(a.destination_x, a.source_x, a.width,
                             int64_t(a.source_y) - a.destination_y, a.destination_y) <
             std::make_tuple(b.destination_x, b.source_x, b.width,
                             int64_t(b.source_y) - b.destination_y, b.destination_y);
    });
    count = 0;
    for (const Region& region : regions) {
      if (count) {
        Region& previous = regions[count - 1];
        if (previous.destination_x == region.destination_x &&
            previous.source_x == region.source_x && previous.width == region.width &&
            uint64_t(previous.destination_y) + previous.height == region.destination_y &&
            uint64_t(previous.source_y) + previous.height == region.source_y) {
          previous.height += region.height;
          continue;
        }
      }
      regions[count++] = region;
    }
    regions.resize(count);
  } while (regions.size() != previous_count);
}

// Both surfaces must have identical pixel format, MSAA and resolution scale.
// A rectangle may cross source row boundaries when base or pitch differs.
// Split at tile rows and source row boundaries; merge adjacent vertical runs
// only if every source and destination coordinate remains consecutive.
inline bool BuildRegions(const Surface& source, const Surface& destination,
                         uint32_t tile_width, uint32_t tile_height,
                         const Rectangle* rectangles, uint32_t rectangle_count,
                         std::vector<Region>& regions, size_t limit = 4096) {
  regions.clear();
  if (!source.pitch_tiles || !destination.pitch_tiles ||
      !tile_width || !tile_height || (!rectangles && rectangle_count)) return false;
  for (uint32_t i = 0; i < rectangle_count; ++i) {
    const auto& rectangle = rectangles[i];
    if (!rectangle.width || !rectangle.height ||
        uint64_t(rectangle.x) + rectangle.width > destination.width ||
        uint64_t(rectangle.y) + rectangle.height > destination.height) {
      regions.clear();
      return false;
    }
    const uint32_t x_end = rectangle.x + rectangle.width;
    const uint32_t y_end = rectangle.y + rectangle.height;
    for (uint32_t y = rectangle.y; y < y_end;) {
      const uint32_t height = std::min(y_end - y, tile_height - y % tile_height);
      for (uint32_t x = rectangle.x; x < x_end;) {
        const int64_t source_tile = int64_t(destination.base_tiles) - source.base_tiles +
            int64_t(y / tile_height) * destination.pitch_tiles + x / tile_width;
        if (source_tile < 0) { regions.clear(); return false; }
        const uint64_t source_x = uint64_t(source_tile % source.pitch_tiles) * tile_width +
                                  x % tile_width;
        const uint64_t source_y = uint64_t(source_tile / source.pitch_tiles) * tile_height +
                                  y % tile_height;
        const uint64_t source_row_remaining =
            uint64_t(source.pitch_tiles - source_tile % source.pitch_tiles) * tile_width -
            x % tile_width;
        const uint32_t width = uint32_t(std::min(uint64_t(x_end - x), source_row_remaining));
        if (!width || source_x + width > source.width ||
            source_y + height > source.height) { regions.clear(); return false; }
        bool merged = false;
        // Search preceding runs for exactly consecutive source and destination rows.
        for (size_t j = regions.size(); j > 0; --j) {
          Region& previous = regions[j - 1];
          if (previous.destination_y + previous.height < y) break;
          if (previous.destination_x == x && previous.source_x == source_x &&
              previous.width == width && previous.destination_y + previous.height == y &&
              previous.source_y + previous.height == source_y) {
            previous.height += height;
            merged = true;
            break;
          }
        }
        if (!merged) {
          if (regions.size() == limit) { regions.clear(); return false; }
          regions.push_back({uint32_t(source_x), uint32_t(source_y), x, y, width, height});
        }
        x += width;
      }
      y += height;
    }
  }
  // A single rectangle is already maximal. Multiple cutout rectangles may
  // produce adjacent runs at their boundaries; combine those independently of
  // the rectangle ordering before passing them to the driver.
  if (rectangle_count > 1 && regions.size() > 1) CoalesceRegions(regions);
  return !regions.empty();
}

}  // namespace rex::graphics::vulkan::ownership_copy
