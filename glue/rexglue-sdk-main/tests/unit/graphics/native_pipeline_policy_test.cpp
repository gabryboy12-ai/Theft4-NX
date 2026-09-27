#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "graphics/gta4_native/native_pipeline_policy.h"

namespace rex::graphics::gta4_native {
namespace {

VkPhysicalDeviceLimits ViewportLimits() {
  VkPhysicalDeviceLimits limits{};
  limits.maxViewportDimensions[0] = 4096;
  limits.maxViewportDimensions[1] = 4096;
  limits.viewportBoundsRange[0] = -8192.0f;
  limits.viewportBoundsRange[1] = 8191.0f;
  return limits;
}

TEST_CASE("Attachmentless fallback extents obey framebuffer limits without scaling viewport") {
  const VkExtent2D extent = BoundNativeAttachmentlessExtent(8192, 4096, 4096, 2048);
  REQUIRE(extent.width == 4096);
  REQUIRE(extent.height == 2048);
  const VkExtent2D ordinary = BoundNativeAttachmentlessExtent(640, 480, 4096, 2048);
  CHECK(ordinary.width == 640);
  CHECK(ordinary.height == 480);
  CHECK(BoundNativeAttachmentlessExtent(0, 480, 4096, 2048).width == 0);
  CHECK(BoundNativeAttachmentlessExtent(640, 0, 4096, 2048).height == 0);

  const std::array<float, 6> requested = {16, 8, 4000, 2000, 0, 1};
  const auto viewport = SelectNativeViewport(requested, 1, 1, extent.width, extent.height,
                                              ViewportLimits());
  REQUIRE_FALSE(viewport.empty);
  CHECK(viewport.viewport.x == requested[0]);
  CHECK(viewport.viewport.y == requested[1]);
  CHECK(viewport.viewport.width == requested[2]);
  CHECK(viewport.viewport.height == requested[3]);
}

TEST_CASE("Native PSO topology and restart follow effective host state",
          "[gta4-native][graphics][pipeline]") {
  CHECK(NativePrimitiveTopology(4) == NativePrimitiveTopology(13));
  CHECK(NativePrimitiveTopology(6) == NativePrimitiveTopology(8));
  CHECK(NativePrimitiveTopology(5) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
  CHECK(NativePrimitiveTopology(0) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM);
  CHECK_FALSE(NativeEffectivePrimitiveRestart(NativePrimitiveTopology(4), true, true));
  CHECK_FALSE(NativeEffectivePrimitiveRestart(NativePrimitiveTopology(6), false, false));
  CHECK(NativeEffectivePrimitiveRestart(NativePrimitiveTopology(6), false, true));
  CHECK(NativeEffectivePrimitiveRestart(NativePrimitiveTopology(5), true, false));
}

TEST_CASE("Native vertex layouts respect alignment and complete attribute extents",
          "[gta4-native][graphics][pipeline]") {
  CHECK(NativeDefaultVertexStride(1, 2048) == 16);
  CHECK(NativeDefaultVertexStride(3, 2048) == 18);
  CHECK(NativeDefaultVertexStride(32, 2048) == 32);
  CHECK(NativeDefaultVertexStride(4096, 2048) == 0);
  CHECK(NativeDefaultVertexStride(UINT32_MAX, 2048) == 0);
  CHECK_FALSE(NativeVertexBindingValid(0, 1, 2048));
  CHECK_FALSE(NativeVertexBindingValid(18, 4, 2048));
  CHECK(NativeVertexBindingValid(20, 4, 2048));
  CHECK_FALSE(NativeVertexBindingValid(4096, 4, 2048));
  CHECK(NativeVertexAttributeFits(12, NativeVertexFormatSize(VK_FORMAT_R32_SFLOAT), 16, false));
  CHECK_FALSE(NativeVertexAttributeFits(12, NativeVertexFormatSize(VK_FORMAT_R32G32_SFLOAT),
                                       16, false));
  CHECK(NativeVertexAttributeFits(12, NativeVertexFormatSize(VK_FORMAT_R32G32_SFLOAT),
                                  16, true));
  CHECK_FALSE(NativeVertexAttributeFits(UINT32_MAX, 16, UINT32_MAX, false));
  CHECK_FALSE(NativeVertexAttributeFits(0, NativeVertexFormatSize(VK_FORMAT_UNDEFINED), 16, true));
  CHECK(NativeVertexFormatSize(VK_FORMAT_R32G32B32A32_SFLOAT) <= kNativeDefaultVertexRecordSize);
  CHECK(NativeVertexFormatSize(VK_FORMAT_R32G32B32A32_SINT) <= kNativeDefaultVertexRecordSize);
  CHECK(NativeVertexFormatSize(VK_FORMAT_R32G32B32A32_UINT) <= kNativeDefaultVertexRecordSize);
}

TEST_CASE("Native empty and nonfinite viewports emit a legal viewport and empty scissor policy",
          "[gta4-native][graphics][pipeline]") {
  const auto limits = ViewportLimits();
  auto requested = std::array<float, 6>{640, 0, 100, 100, 0, 1};
  const auto edge = SelectNativeViewport(requested, 1, 1, 640, 480, limits);
  CHECK(edge.empty);
  CHECK(edge.viewport.width == 1.0f);
  CHECK(edge.viewport.height == 1.0f);
  requested[0] = 639;
  const auto clipped = SelectNativeViewport(requested, 1, 1, 640, 480, limits);
  CHECK_FALSE(clipped.empty);
  CHECK(clipped.viewport.x == 639.0f);
  CHECK(clipped.viewport.width == 100.0f);
  requested = {-20, 0, 100, 100, 0, 1};
  const auto partial = SelectNativeViewport(requested, 2, 1, 640, 480, limits);
  CHECK_FALSE(partial.empty);
  CHECK(partial.viewport.x == -40.0f);
  CHECK(partial.viewport.width == 200.0f);
  requested = {-100, 0, 50, 100, 0, 1};
  CHECK(SelectNativeViewport(requested, 1, 1, 640, 480, limits).empty);
  requested = {0, 0, 100, 100, 0, 1};
  for (size_t component = 0; component < requested.size(); ++component) {
    auto invalid = requested;
    invalid[component] = std::numeric_limits<float>::quiet_NaN();
    CHECK(SelectNativeViewport(invalid, 1, 1, 640, 480, limits).empty);
    invalid[component] = std::numeric_limits<float>::infinity();
    CHECK(SelectNativeViewport(invalid, 1, 1, 640, 480, limits).empty);
  }
  requested[2] = 0;
  CHECK(SelectNativeViewport(requested, 1, 1, 640, 480, limits).empty);
}

TEST_CASE("Native viewport scaling preserves reversed depth and enforces device limits",
          "[gta4-native][graphics][pipeline]") {
  auto limits = ViewportLimits();
  const auto requested = std::array<float, 6>{20, 30, 100, 50, 1, 0};
  const auto scaled = SelectNativeViewport(requested, 2, 3, 1280, 1440, limits);
  CHECK_FALSE(scaled.empty);
  CHECK(scaled.viewport.x == 40);
  CHECK(scaled.viewport.y == 90);
  CHECK(scaled.viewport.width == 200);
  CHECK(scaled.viewport.height == 150);
  CHECK(scaled.viewport.minDepth == 1);
  CHECK(scaled.viewport.maxDepth == 0);
  limits.maxViewportDimensions[0] = 100;
  CHECK(SelectNativeViewport(requested, 2, 3, 1280, 1440, limits).empty);
  CHECK(NativeResolutionDepthBiasScale(1, 1) == 1);
  CHECK(NativeResolutionDepthBiasScale(2, 2) == 2);
  CHECK(NativeResolutionDepthBiasScale(2, 3) == 3);
}

TEST_CASE("Native constant alpha fallback preserves each written color channel",
          "[gta4-native][graphics][pipeline]") {
  std::array<uint32_t, 4> controls{0x0001000Eu, 0, 0, 0};
  const std::array<float, 4> constants{0.25f, 0.5f, 0.75f, 1.0f};
  const auto alpha = SelectNativeBlendConstants(controls, 0xFu, constants, false);
  CHECK(alpha.representable);
  CHECK(alpha.constants == std::array<float, 4>{1, 1, 1, 1});
  const auto native = SelectNativeBlendConstants(controls, 0xFu, constants, true);
  CHECK(native.constants == constants);
  controls[0] = 0x00010C0Eu;
  CHECK_FALSE(SelectNativeBlendConstants(controls, 0xFu, constants, false).representable);
  CHECK(SelectNativeBlendConstants(controls, 0x8u, constants, false).representable);
  controls = {0x0001000Eu, 0x0001000Cu, 0, 0};
  // Different attachments can share the remap if their written channels do not overlap.
  const auto disjoint = SelectNativeBlendConstants(controls, 0x21u, constants, false);
  CHECK(disjoint.representable);
  CHECK(disjoint.constants == std::array<float, 4>{1, 0.5f, 0.75f, 1});
  CHECK_FALSE(SelectNativeBlendConstants(controls, 0x11u, constants, false).representable);
  const std::array<VkFormat, 4> formats = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
  CHECK(NativeBlendWriteMask(0xFFu, formats) == 0xF1u);
  // Normalization marks absent G/B/A channels writable on R32 for the host
  // fast path. They must not conflict with real color constants on another MRT.
  CHECK(SelectNativeBlendConstants(controls, NativeBlendWriteMask(0x2Fu, formats),
                                    constants, false).representable);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
