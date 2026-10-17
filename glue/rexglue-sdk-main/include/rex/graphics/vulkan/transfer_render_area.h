#pragma once

// Preserve every transfer and clear pixel while limiting attachment operations
// to the union of their known destination rectangles. Independent of Vulkan so
// the bounds used by both render-pass APIs can be checked on the host.
#include <algorithm>
#include <cstdint>

namespace rex::graphics::vulkan::transfer_render_area {

struct Rectangle {
  uint32_t x, y, width, height;
};

class Bounds {
 public:
  Bounds(uint32_t width, uint32_t height) : full_{0, 0, width, height} {}

  // Pixel rectangles from ownership transfers are unsigned and bounded by the
  // destination surface. Resolve clears use the same scaled coordinate space.
  void Include(const Rectangle& rectangle) {
    if (!rectangle.width || !rectangle.height) return;
    const uint64_t x1 = uint64_t(rectangle.x) + rectangle.width;
    const uint64_t y1 = uint64_t(rectangle.y) + rectangle.height;
    // A caller mismatch must preserve the original full-area pass, rather than
    // silently narrowing a pass around an unrepresented clear or draw.
    if (x1 > full_.width || y1 > full_.height) {
      invalid_ = true;
      return;
    }
    if (!has_bounds_) {
      bounds_ = rectangle;
      has_bounds_ = true;
      return;
    }
    const uint32_t union_x = std::min(bounds_.x, rectangle.x);
    const uint32_t union_y = std::min(bounds_.y, rectangle.y);
    const uint32_t union_x1 = uint32_t(std::max(
        uint64_t(bounds_.x) + bounds_.width, x1));
    const uint32_t union_y1 = uint32_t(std::max(
        uint64_t(bounds_.y) + bounds_.height, y1));
    bounds_ = {union_x, union_y, union_x1 - union_x, union_y1 - union_y};
  }

  void IncludeScaled(const Rectangle& rectangle, uint32_t scale_x, uint32_t scale_y) {
    if (!rectangle.width || !rectangle.height) return;
    const uint64_t x = uint64_t(rectangle.x) * scale_x;
    const uint64_t y = uint64_t(rectangle.y) * scale_y;
    const uint64_t width = uint64_t(rectangle.width) * scale_x;
    const uint64_t height = uint64_t(rectangle.height) * scale_y;
    if (!scale_x || !scale_y || x > UINT32_MAX || y > UINT32_MAX ||
        width > UINT32_MAX || height > UINT32_MAX) {
      invalid_ = true;
      return;
    }
    Include({uint32_t(x), uint32_t(y), uint32_t(width), uint32_t(height)});
  }

  Rectangle Get() const { return invalid_ || !has_bounds_ ? full_ : bounds_; }

 private:
  Rectangle full_, bounds_{};
  bool has_bounds_ = false;
  bool invalid_ = false;
};

}  // namespace rex::graphics::vulkan::transfer_render_area
