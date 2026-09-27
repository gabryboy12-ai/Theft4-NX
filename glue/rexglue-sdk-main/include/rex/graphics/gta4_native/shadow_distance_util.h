#ifndef REX_GRAPHICS_GTA4_NATIVE_SHADOW_DISTANCE_UTIL_H_
#define REX_GRAPHICS_GTA4_NATIVE_SHADOW_DISTANCE_UTIL_H_

#include <array>
#include <cmath>
#include <cstddef>

namespace rex::graphics::gta4_native {

// sub_82274BE8 initializes one 256-byte shadow context, then sub_822731E0
// copies that complete context to the immediately following slot.
constexpr size_t kNativeShadowContextCount = 2;

inline bool CalculateNativeShadowRanges(
    const std::array<float, kNativeShadowContextCount>& original_ranges,
    double configured_scale,
    std::array<float, kNativeShadowContextCount>& scaled_ranges) {
  if (!std::isfinite(configured_scale) || configured_scale <= 0.0) {
    return false;
  }

  std::array<float, kNativeShadowContextCount> candidate_ranges{};
  const float scale = static_cast<float>(configured_scale);
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return false;
  }
  for (size_t context = 0; context < original_ranges.size(); ++context) {
    const float original = original_ranges[context];
    const float candidate = original * scale;
    if (!std::isfinite(original) || original <= 0.0f ||
        !std::isfinite(candidate) || candidate <= 0.0f) {
      return false;
    }
    candidate_ranges[context] = candidate;
  }

  scaled_ranges = candidate_ranges;
  return true;
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_SHADOW_DISTANCE_UTIL_H_
