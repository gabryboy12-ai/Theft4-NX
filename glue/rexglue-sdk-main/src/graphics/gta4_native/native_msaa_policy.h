#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics::gta4_native {

// The title deliberately switches from multisampled deferred attachments to
// single-sampled persistent/forward attachments. A host MSAA quality override
// may change the sample count of the former, but must never promote the latter:
// doing so changes the depth/stencil handoff topology and invalidates the
// title-authored stencil contents used by the scene composite.
constexpr bool ShouldApplyNativeSceneSampleOverride(
    bool reflection_target, bool has_bound_surface,
    bool all_bound_surfaces_are_guest_multisampled) {
  return !reflection_target && has_bound_surface &&
         all_bound_surfaces_are_guest_multisampled;
}

// VkSampleCountFlagBits values are the corresponding power-of-two sample
// counts. Select the requested count when supported, otherwise the greatest
// coherent lower count. Returning zero means even the mandatory 1x fallback
// was not advertised and surface creation must fail explicitly.
constexpr uint32_t SelectSupportedNativeSceneSampleCount(
    uint32_t requested, uint32_t supported) {
  if (requested >= 4u && (supported & 4u)) {
    return 4u;
  }
  if (requested >= 2u && (supported & 2u)) {
    return 2u;
  }
  return supported & 1u;
}

constexpr xenos::CopySampleSelect ResolveAllSamples(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k4X:
      return xenos::CopySampleSelect::k0123;
    case xenos::MsaaSamples::k2X:
      return xenos::CopySampleSelect::k01;
    default:
      return xenos::CopySampleSelect::k0;
  }
}

constexpr bool IsResolveAllSamples(xenos::CopySampleSelect sample_select,
                                   xenos::MsaaSamples samples) {
  return sample_select == ResolveAllSamples(samples);
}

struct NativeColorResolveSampleMapping {
  xenos::MsaaSamples content_samples = xenos::MsaaSamples::k1X;
  xenos::MsaaSamples requested_samples = xenos::MsaaSamples::k1X;
  xenos::CopySampleSelect sample_select = xenos::CopySampleSelect::k0;
  bool expanded_to_physical_samples = false;
};

// A host quality override changes the number of samples physically stored in
// an image without changing the title's guest attachment view. When the title asks
// to resolve every sample from the same guest view, resolve every physical
// host sample instead. Explicit sample selections and placement reinterpretations
// retain their guest topology and are mapped by the conversion shader.
constexpr NativeColorResolveSampleMapping NormalizeColorResolveSampleMapping(
    xenos::MsaaSamples content_samples, xenos::MsaaSamples requested_samples,
    xenos::MsaaSamples physical_samples, xenos::CopySampleSelect sample_select) {
  NativeColorResolveSampleMapping result{content_samples, requested_samples, sample_select, false};
  if (physical_samples != content_samples && content_samples == requested_samples &&
      IsResolveAllSamples(sample_select, requested_samples)) {
    result.content_samples = physical_samples;
    result.requested_samples = physical_samples;
    result.sample_select = ResolveAllSamples(physical_samples);
    result.expanded_to_physical_samples = true;
  }
  return result;
}

constexpr bool CanDirectlyMaterializePhysicalSamples(bool same_guest_view,
                                                     uint32_t source_samples,
                                                     uint32_t destination_samples) {
  return same_guest_view && source_samples == destination_samples;
}

}  // namespace rex::graphics::gta4_native
