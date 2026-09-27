/**
 * @file native_clip_control_test.cpp
 * @brief GTA IV native-renderer clip-control translation tests.
 */

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_clip_control.h"
#include "graphics/gta4_native/native_fixed_function_policy.h"
#include "graphics/gta4_native/native_stencil_volume_policy.h"

namespace rex::graphics::gta4_native {

TEST_CASE("GTA IV native clip-disable state requests Vulkan depth clamp",
          "[gta4-native][graphics][clip-control]") {
  CHECK_FALSE(IsNativeDepthClampRequested(0u));
  CHECK(IsNativeDepthClampRequested(kNativeClipDisableMask));
  CHECK_FALSE(IsNativeDepthClampRequested(1u << 19));
  CHECK(IsNativeDepthClampRequested(kNativeClipDisableMask | (1u << 19) | 0x3Fu));
}

TEST_CASE("GTA IV native clip control preserves clip-space and user-plane fields",
          "[gta4-native][graphics][clip-control]") {
  CHECK(IsNativeNegativeOneToOneClipSpace(0u));
  CHECK_FALSE(IsNativeNegativeOneToOneClipSpace(kNativeDirectXClipSpaceMask));
  CHECK(NativeUserClipPlaneEnableMask(0xFFFFFFFFu) == 0x3Fu);
  CHECK_FALSE(HasUnsupportedNativeUserClipPlanes(0u));
  CHECK_FALSE(HasUnsupportedNativeUserClipPlanes(1u));
  CHECK(HasUnsupportedNativeUserClipPlanes(1u | kNativeUserClipCullOnlyMask));
  CHECK(HasUnsupportedNativeUserClipPlanes(2u));
  CHECK(HasUnsupportedNativeUserClipPlanes(0x3Fu));
}

TEST_CASE("GTA IV two-sided z-fail light volumes retain depth-clip closure",
          "[gta4-native][graphics][stencil-volume]") {
  NativeStencilVolumeDepthState state{};
  state.depth_enable = 1;
  state.stencil_enable = 1;
  state.two_sided_stencil = 1;
  state.cull_mode = 0;
  state.stencil_write_mask = 0xFF;
  state.back_stencil_write_mask = 0xFF;
  state.ccw_stencil_depth_fail = 7;

  CHECK(IsNativeTwoSidedZFailLightVolume(state));
  CHECK(ShouldEnableNativeDepthClampForDraw(false, true, state));
  CHECK_FALSE(ShouldEnableNativeDepthClampForDraw(false, false, state));

  SECTION("writes on unbound render targets do not suppress light-volume closure") {
    state.active_color_write = AnyNativeColorWriteEnabledForTargets(0xFFF0u, 0b0001u);
    CHECK_FALSE(state.active_color_write);
    CHECK(IsNativeTwoSidedZFailLightVolume(state));
  }

  SECTION("guest clamp remains authoritative") {
    state.active_color_write = true;
    CHECK(ShouldEnableNativeDepthClampForDraw(true, false, state));
  }

  SECTION("color accumulation is not classified as a stencil volume") {
    state.active_color_write = true;
    CHECK_FALSE(IsNativeTwoSidedZFailLightVolume(state));
  }

  SECTION("z-pass stencil work is not classified as z-fail") {
    state.ccw_stencil_depth_fail = 0;
    state.ccw_stencil_pass = 7;
    CHECK_FALSE(IsNativeTwoSidedZFailLightVolume(state));
  }

  SECTION("one-sided or culled setup draws retain title clipping") {
    state.two_sided_stencil = 0;
    CHECK_FALSE(IsNativeTwoSidedZFailLightVolume(state));
    state.two_sided_stencil = 1;
    state.cull_mode = 2;
    CHECK_FALSE(IsNativeTwoSidedZFailLightVolume(state));
  }

  SECTION("depth-writing geometry retains title clipping") {
    state.depth_write_enable = 1;
    CHECK_FALSE(IsNativeTwoSidedZFailLightVolume(state));
  }

  SECTION("either writable face can establish z-fail volume state") {
    state.ccw_stencil_depth_fail = 0;
    state.back_stencil_write_mask = 0;
    state.stencil_depth_fail = 6;
    CHECK(IsNativeTwoSidedZFailLightVolume(state));
  }
}

}  // namespace rex::graphics::gta4_native
