#ifndef REX_GRAPHICS_GTA4_NATIVE_ALPHA_TO_COVERAGE_UTIL_H_
#define REX_GRAPHICS_GTA4_NATIVE_ALPHA_TO_COVERAGE_UTIL_H_

#include <cstdint>

namespace rex::graphics::gta4_native {

constexpr uint32_t kNativeAlphaToMaskEnable = 1u << 8;

// RB_COLORCONTROL stores the four 2-bit alpha-to-mask offsets in its upper
// byte. The native shaders consume a compact native-only layout: bits 0-7 are
// the Xenos offsets and bit 8 is enable.
constexpr uint32_t PackNativeAlphaToMask(uint32_t color_control) {
  return color_control & 0x10u
             ? ((color_control >> 24) & 0xFFu) | kNativeAlphaToMaskEnable
             : 0u;
}

constexpr bool IsNativeAlphaToMaskRequested(uint32_t packed_alpha_to_mask) {
  return (packed_alpha_to_mask & kNativeAlphaToMaskEnable) != 0;
}

constexpr uint32_t ComputeNativeAlphaToMask(float alpha, uint32_t pixel_x,
                                                  uint32_t pixel_y,
                                                  uint32_t packed_alpha_to_mask,
                                                  uint32_t sample_count) {
  if (!IsNativeAlphaToMaskRequested(packed_alpha_to_mask)) {
    return 0xFFFFFFFFu;
  }

  const uint32_t offset_index = (pixel_x & 1u) | ((pixel_y & 1u) << 1u);
  const uint32_t offset = (packed_alpha_to_mask >> (offset_index << 1u)) & 3u;
  const float threshold_offset = static_cast<float>(offset);
  uint32_t sample_mask = 0;
  if (sample_count == 1) {
    if (alpha >= 1.0f - threshold_offset * 0.25f) {
      sample_mask |= 1u;
    }
  } else if (sample_count == 2) {
    if (alpha >= 0.5f - threshold_offset * 0.125f) {
      sample_mask |= 2u;
    }
    if (alpha >= 1.0f - threshold_offset * 0.125f) {
      sample_mask |= 1u;
    }
  } else if (sample_count == 4) {
    if (alpha >= 0.75f - threshold_offset * 0.0625f) {
      sample_mask |= 1u;
    }
    if (alpha >= 0.25f - threshold_offset * 0.0625f) {
      sample_mask |= 4u;
    }
    if (alpha >= 0.5f - threshold_offset * 0.0625f) {
      sample_mask |= 2u;
    }
    if (alpha >= 1.0f - threshold_offset * 0.0625f) {
      sample_mask |= 8u;
    }
  } else {
    return 0xFFFFFFFFu;
  }
  return sample_mask;
}


constexpr bool IsNativeFragmentCoverageRequested(bool alpha_test_requested,
                                                  uint32_t packed_alpha_to_mask) {
  return alpha_test_requested || IsNativeAlphaToMaskRequested(packed_alpha_to_mask);
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_ALPHA_TO_COVERAGE_UTIL_H_
