#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_aspect_content.h"
#include "graphics/gta4_native/native_attachment_policy.h"
#include "graphics/gta4_native/native_binding_policy.h"
#include "graphics/gta4_native/native_host_enhancement_policy.h"
#include "graphics/gta4_native/native_lighting_lineage.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("Lighting attachments use output and component masks before allocation",
          "[gta4-native][lighting][attachments]") {
  const auto stencil = SelectNativeDrawAttachmentUsage(0xFFF0u, 1u, 0xFFFFu, true, true);
  CHECK(stencil.color_attachment_mask == 0);
  CHECK(stencil.color_write_mask == 0);
  CHECK(stencil.depth_stencil_aspects ==
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
  const auto mrt = SelectNativeDrawAttachmentUsage(0xFFFFu, 4u, 0xFFFFu, false, false);
  CHECK(mrt.color_attachment_mask == 4u);
  CHECK(mrt.color_write_mask == 0x0F00u);
  const auto absent_component = SelectNativeDrawAttachmentUsage(8u, 1u, 1u, true, false);
  CHECK(absent_component.color_attachment_mask == 0);
  CHECK(absent_component.depth_stencil_aspects == VK_IMAGE_ASPECT_DEPTH_BIT);
}

TEST_CASE("Explicit clears preserve attachment indices and independent aspects",
          "[gta4-native][lighting][attachments]") {
  CHECK(SelectNativeClearAttachmentUsage(2u).color_attachment_mask == 2u);
  CHECK(SelectNativeClearAttachmentUsage(2u).color_write_mask == 0x00F0u);
  CHECK(SelectNativeClearAttachmentUsage(0x10u).depth_stencil_aspects == VK_IMAGE_ASPECT_DEPTH_BIT);
  CHECK(SelectNativeClearAttachmentUsage(0x20u).depth_stencil_aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
  CHECK(SelectNativeClearAttachmentUsage(0u).depth_stencil_aspects == 0);
}

TEST_CASE("Resolve clear attachment routing depends only on image aspects",
          "[gta4-native][lighting][attachments]") {
  VkRenderingAttachmentInfo depth{};
  VkRenderingAttachmentInfo stencil{};
  VkRenderingInfo rendering{};
  SetNativeRenderingAttachmentBindings(rendering,
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, depth, stencil);
  CHECK(rendering.colorAttachmentCount == 0);
  CHECK(rendering.pColorAttachments == nullptr);
  CHECK(rendering.pDepthAttachment == &depth);
  CHECK(rendering.pStencilAttachment == &stencil);
  SetNativeRenderingAttachmentBindings(rendering, VK_IMAGE_ASPECT_COLOR_BIT, depth, stencil);
  CHECK(rendering.colorAttachmentCount == 1);
  CHECK(rendering.pColorAttachments == &depth);
  CHECK(rendering.pDepthAttachment == nullptr);
  CHECK(rendering.pStencilAttachment == nullptr);
  SetNativeRenderingAttachmentBindings(rendering, VK_IMAGE_ASPECT_DEPTH_BIT, depth, stencil);
  CHECK(rendering.pDepthAttachment == &depth);
  CHECK(rendering.pStencilAttachment == nullptr);
}

TEST_CASE("Required binding failures are distinct from null and unused bindings",
          "[gta4-native][lighting][bindings]") {
  CHECK(SelectNativeBindingRealization(false, true, false, false, false) ==
        NativeBindingRealization::kUnused);
  CHECK(SelectNativeBindingRealization(true, false, false, false, false) ==
        NativeBindingRealization::kGuestNull);
  CHECK(SelectNativeBindingRealization(true, true, false, false, false) ==
        NativeBindingRealization::kSnapshotMissing);
  CHECK(SelectNativeBindingRealization(true, true, true, false, false) ==
        NativeBindingRealization::kImageFailed);
  CHECK(SelectNativeBindingRealization(true, true, true, true, false) ==
        NativeBindingRealization::kSamplerFailed);
  CHECK(SelectNativeBindingRealization(true, true, true, true, true) ==
        NativeBindingRealization::kReady);
  CHECK_FALSE(NativeBindingFailed(NativeBindingRealization::kGuestNull));
  CHECK_FALSE(NativeBindingFailed(NativeBindingRealization::kUnused));
  CHECK(NativeBindingFailed(NativeBindingRealization::kSnapshotMissing));
  CHECK(NativeBindingFailed(NativeBindingRealization::kImageFailed));
  CHECK(NativeBindingFailed(NativeBindingRealization::kSamplerFailed));
}

TEST_CASE("Missing guest pixel shader handle cannot masquerade as a depth-only draw",
          "[gta4-native][lighting][bindings]") {
  CHECK(NativeMissingPixelShaderIsIntentional(0, true, false));
  CHECK_FALSE(NativeMissingPixelShaderIsIntentional(7, true, false));
  CHECK_FALSE(NativeMissingPixelShaderIsIntentional(0, false, false));
  CHECK_FALSE(NativeMissingPixelShaderIsIntentional(0, true, true));
}

TEST_CASE("Depth handoff and stencil setup update only the aspect actually written",
          "[gta4-native][lighting][content]") {
  NativeSurfaceAspectContent destination{};
  destination.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 10);
  const NativeAspectContent stencil = destination.stencil;
  destination.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 20, 3);
  CHECK(destination.stencil == stencil);
  CHECK(destination.depth.ancestor_serial == 3);
  const NativeAspectContent depth = destination.depth;
  destination.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 30);
  CHECK(destination.depth == depth);
  CHECK(destination.stencil.ancestor_serial == 10);
  CHECK_FALSE(destination.Has(VK_IMAGE_ASPECT_COLOR_BIT));
  CHECK_FALSE(destination.Has(VK_IMAGE_ASPECT_METADATA_BIT));
  CHECK_FALSE(destination.Has(0));
}

TEST_CASE("Partial depth-stencil clear independently preserves defined depth",
          "[gta4-native][lighting][content]") {
  NativeSurfaceAspectContent content{};
  content.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 1);
  const auto depth = PlanNativeAspectClear(content.depth, true, false);
  const auto stencil = PlanNativeAspectClear(content.stencil, true, false);
  CHECK(depth.permitted);
  CHECK(depth.load == VK_ATTACHMENT_LOAD_OP_LOAD);
  CHECK(stencil.permitted);
  CHECK(stencil.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
  CHECK_FALSE(PlanNativeAspectClear(content.depth, false, false).permitted);
  CHECK(PlanNativeAspectClear(content.depth, false, true).load == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
}

TEST_CASE("Depth-only conversion requires no stencil and preserves destination stencil ancestry",
          "[gta4-native][lighting][content]") {
  NativeSurfaceAspectContent source{};
  source.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 5);
  CHECK(source.Has(VK_IMAGE_ASPECT_DEPTH_BIT));
  CHECK_FALSE(source.Has(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
  NativeSurfaceAspectContent destination{};
  destination.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 7);
  const NativeAspectContent stencil = destination.stencil;
  destination.CopyFrom(source, VK_IMAGE_ASPECT_DEPTH_BIT, 9);
  CHECK(destination.depth.ancestor_serial == 5);
  CHECK(destination.stencil == stencil);
}

TEST_CASE("Same-image lighting lineage rejects intervening aspect clears",
          "[gta4-native][lighting][content]") {
  NativeSurfaceAspectContent content{};
  content.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 1);
  content.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 2);
  // Captured after the setup stencil write, before contribution.
  NativeLightingAttachmentLineage setup{1, 2, 3, 1, 1, 64, 64, content.depth, content.stencil};
  CHECK(NativeLightingAttachmentUnchanged(setup, setup));
  auto contribution = setup;
  content.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 3);
  contribution.stencil = content.stencil;
  CHECK_FALSE(NativeLightingAttachmentUnchanged(setup, contribution));
  contribution = setup;
  content.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 4);
  contribution.depth = content.depth;
  CHECK_FALSE(NativeLightingAttachmentUnchanged(setup, contribution));
  contribution = setup;
  contribution.lifetime = 9;
  CHECK_FALSE(NativeLightingAttachmentUnchanged(setup, contribution));
}

TEST_CASE("Host fog override control leaves unrelated compatibility overrides enabled",
          "[gta4-native][lighting][enhancements]") {
  CHECK_FALSE(AllowNativeHostOverride(0xEFAACEED3DBD802DULL, false));
  CHECK_FALSE(AllowNativeHostOverride(0x1A6669BBAFDC43E7ULL, false));
  CHECK(AllowNativeHostOverride(0xEFAACEED3DBD802DULL, true));
  CHECK(AllowNativeHostOverride(1, false));
}

TEST_CASE("Lighting lineage survives internal submission but not a guest frame boundary",
          "[gta4-native][lighting][content]") {
  LightingContext setup{};
  setup.stage = RenderExecutionStage::kDeferredLighting;
  setup.role = LightPassRole::kLocalStencilSetup;
  setup.source = LightSourceKind::kPrimaryRecord;
  setup.source_function = 0x822B0F08;
  setup.requested_selector = 3;
  setup.record_address = 100;
  setup.occurrence_id = 7;
  setup.view_id = 8;
  setup.view_address = 200;
  setup.stencil_setup_expected = 1;
  LightingContext contribution = setup;
  contribution.role = LightPassRole::kLocalContribution;
  contribution.requested_selector = 4;
  NativeSurfaceAspectContent content{};
  content.Write(VK_IMAGE_ASPECT_DEPTH_BIT, 1);
  content.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 2);
  NativeLightingAttachmentLineage attachment{1, 2, 3, 1, 1, 64, 64,
                                            content.depth, content.stencil};
  NativeLightingLineageLedger ledger;
  ledger.RecordSetup(setup, attachment);
  CHECK(ledger.Matches(contribution, attachment));
  ledger.OnBoundary(NativeLightingBatchBoundary::kInternalFlush);
  CHECK(ledger.Matches(contribution, attachment));
  content.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 3);
  attachment.stencil = content.stencil;
  CHECK_FALSE(ledger.Matches(contribution, attachment));
  ledger.RecordContribution(contribution, attachment);
  CHECK(ledger.Matches(contribution, attachment));
  ledger.OnBoundary(NativeLightingBatchBoundary::kGuestPresent);
  CHECK_FALSE(ledger.Matches(contribution, attachment));
  ledger.RecordSetup(setup, attachment);
  ledger.OnBoundary(NativeLightingBatchBoundary::kDeviceReset);
  CHECK_FALSE(ledger.Matches(contribution, attachment));
  ledger.RecordSetup(setup, attachment);
  ledger.OnBoundary(NativeLightingBatchBoundary::kDiscardedRecording);
  CHECK_FALSE(ledger.Matches(contribution, attachment));
}

}  // namespace
}  // namespace rex::graphics::gta4_native
