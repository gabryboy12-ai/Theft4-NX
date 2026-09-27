/**
 * @file native_stencil_volume_policy.h
 * @brief Host rasterization policy for GTA IV two-sided z-fail light volumes.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

struct NativeStencilVolumeDepthState {
  uint32_t depth_enable = 0;
  uint32_t depth_write_enable = 0;
  uint32_t stencil_enable = 0;
  uint32_t two_sided_stencil = 0;
  uint32_t cull_mode = 0;
  bool active_color_write = false;
  uint32_t stencil_depth_fail = 0;
  uint32_t stencil_pass = 0;
  uint32_t stencil_write_mask = 0;
  uint32_t ccw_stencil_depth_fail = 0;
  uint32_t ccw_stencil_pass = 0;
  uint32_t back_stencil_write_mask = 0;
};

constexpr bool IsNativeNoCullMode(uint32_t cull_mode) {
  return cull_mode == 0 || cull_mode == 4;
}

constexpr bool NativeStencilFaceUsesZFail(uint32_t depth_fail_operation, uint32_t pass_operation,
                                          uint32_t write_mask) {
  return depth_fail_operation != 0 && pass_operation == 0 && write_mask != 0;
}

constexpr bool IsNativeTwoSidedZFailLightVolume(const NativeStencilVolumeDepthState& state) {
  if (!state.depth_enable || state.depth_write_enable || !state.stencil_enable ||
      !state.two_sided_stencil || !IsNativeNoCullMode(state.cull_mode) ||
      state.active_color_write) {
    return false;
  }

  return NativeStencilFaceUsesZFail(state.stencil_depth_fail, state.stencil_pass,
                                    state.stencil_write_mask) ||
         NativeStencilFaceUsesZFail(state.ccw_stencil_depth_fail, state.ccw_stencil_pass,
                                    state.back_stencil_write_mask);
}

constexpr bool ShouldEnableNativeDepthClampForDraw(bool guest_depth_clamp, bool light_setup_phase,
                                                   const NativeStencilVolumeDepthState& state) {
  return guest_depth_clamp || (light_setup_phase && IsNativeTwoSidedZFailLightVolume(state));
}

}  // namespace rex::graphics::gta4_native
