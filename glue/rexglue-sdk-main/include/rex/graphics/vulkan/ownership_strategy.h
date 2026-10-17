#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::graphics::vulkan::ownership_strategy {

// A shader mesh retains the input rectangles and computes their EDRAM source
// address per pixel. Copy-engine or meta-draw work grows with mapped row strips.
// Require both an absolute floor and an eightfold command amplification before
// considering the preflighted shader path; cheap affine copies remain copies.
inline bool HighAmplification(uint64_t mapped_regions, uint64_t rectangles,
                              uint32_t sample_pipelines, bool is_depth) {
  if (!rectangles || !sample_pipelines) return false;
  const uint64_t factor = uint64_t(sample_pipelines) * (is_depth ? 16 : 8);
  if (rectangles > UINT64_MAX / factor) return false;
  const uint64_t shader_draws = rectangles * sample_pipelines * (is_depth ? 2 : 1);
  return mapped_regions > std::max(uint64_t(16), shader_draws * 8);
}

// Keep zero image origin so existing viewport, scissor, NDC and copy coordinates
// remain unchanged. Allocate only through the largest written coordinate.
class ScratchBounds {
 public:
  struct Result { uint32_t x, y, width, height, image_width, image_height; };
  ScratchBounds(uint32_t width, uint32_t height) : width_(width), height_(height) {}
  bool Include(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               uint32_t scale_x, uint32_t scale_y) {
    if (!width || !height || !scale_x || !scale_y) return false;
    if (uint64_t(x) + width > width_ / scale_x ||
        uint64_t(y) + height > height_ / scale_y) return false;
    const uint64_t x0 = uint64_t(x) * scale_x, y0 = uint64_t(y) * scale_y;
    const uint64_t x1 = (uint64_t(x) + width) * scale_x;
    const uint64_t y1 = (uint64_t(y) + height) * scale_y;
    if (x1 > width_ || y1 > height_) return false;
    if (!has_bounds_) { x_ = uint32_t(x0); y_ = uint32_t(y0); has_bounds_ = true; }
    else { x_ = std::min(x_, uint32_t(x0)); y_ = std::min(y_, uint32_t(y0)); }
    end_x_ = std::max(end_x_, uint32_t(x1)); end_y_ = std::max(end_y_, uint32_t(y1));
    return true;
  }
  bool valid() const { return has_bounds_; }
  Result Get() const { return {x_, y_, end_x_ - x_, end_y_ - y_, end_x_, end_y_}; }
 private:
  uint32_t width_, height_, x_ = 0, y_ = 0, end_x_ = 0, end_y_ = 0;
  bool has_bounds_ = false;
};

}  // namespace rex::graphics::vulkan::ownership_strategy
