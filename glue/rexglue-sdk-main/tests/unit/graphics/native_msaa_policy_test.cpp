#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/gta4_native/anti_aliasing_policy.h>
#include <rex/graphics/gta4_native/supersampling_policy.h>

#include "graphics/gta4_native/native_msaa_policy.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("GTA IV native MSAA overrides only title-multisampled scene families") {
  CHECK(ShouldApplyNativeSceneSampleOverride(false, true, true));

  CHECK_FALSE(ShouldApplyNativeSceneSampleOverride(false, true, false));
  CHECK_FALSE(ShouldApplyNativeSceneSampleOverride(false, false, true));
  CHECK_FALSE(ShouldApplyNativeSceneSampleOverride(true, true, true));
}

TEST_CASE("GTA IV native MSAA preserves mixed and forward sample topology") {
  // A family containing any guest 1x attachment is title-authored mixed or
  // forward topology and must remain untouched as a coherent family.
  CHECK_FALSE(ShouldApplyNativeSceneSampleOverride(false, true, false));

  // Reflection families own an independent sample-count policy.
  CHECK_FALSE(ShouldApplyNativeSceneSampleOverride(true, true, true));
}

TEST_CASE("GTA IV native MSAA selects the highest supported requested count") {
  CHECK(SelectSupportedNativeSceneSampleCount(4, 1 | 2 | 4) == 4);
  CHECK(SelectSupportedNativeSceneSampleCount(4, 1 | 2) == 2);
  CHECK(SelectSupportedNativeSceneSampleCount(4, 1) == 1);
  CHECK(SelectSupportedNativeSceneSampleCount(2, 1 | 2 | 4) == 2);
  CHECK(SelectSupportedNativeSceneSampleCount(2, 1) == 1);
  CHECK(SelectSupportedNativeSceneSampleCount(1, 1 | 2 | 4) == 1);
  CHECK(SelectSupportedNativeSceneSampleCount(4, 0) == 0);
}

TEST_CASE("GTA IV native color resolve-all consumes every physical host sample") {
  const NativeColorResolveSampleMapping mapping = NormalizeColorResolveSampleMapping(
      xenos::MsaaSamples::k2X, xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X,
      xenos::CopySampleSelect::k01);
  CHECK(mapping.content_samples == xenos::MsaaSamples::k4X);
  CHECK(mapping.requested_samples == xenos::MsaaSamples::k4X);
  CHECK(mapping.sample_select == xenos::CopySampleSelect::k0123);
  CHECK(mapping.expanded_to_physical_samples);
}

TEST_CASE("GTA IV native color resolve preserves explicit guest sample selection") {
  const NativeColorResolveSampleMapping explicit_sample = NormalizeColorResolveSampleMapping(
      xenos::MsaaSamples::k2X, xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X,
      xenos::CopySampleSelect::k0);
  CHECK(explicit_sample.content_samples == xenos::MsaaSamples::k2X);
  CHECK(explicit_sample.requested_samples == xenos::MsaaSamples::k2X);
  CHECK(explicit_sample.sample_select == xenos::CopySampleSelect::k0);
  CHECK_FALSE(explicit_sample.expanded_to_physical_samples);
}

TEST_CASE("GTA IV native color resolve preserves placement reinterpretation topology") {
  const NativeColorResolveSampleMapping alternate_view = NormalizeColorResolveSampleMapping(
      xenos::MsaaSamples::k4X, xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X,
      xenos::CopySampleSelect::k01);
  CHECK(alternate_view.content_samples == xenos::MsaaSamples::k4X);
  CHECK(alternate_view.requested_samples == xenos::MsaaSamples::k2X);
  CHECK(alternate_view.sample_select == xenos::CopySampleSelect::k01);
  CHECK_FALSE(alternate_view.expanded_to_physical_samples);
}

TEST_CASE("GTA IV native color resolve leaves a matching physical fallback unchanged") {
  const NativeColorResolveSampleMapping fallback = NormalizeColorResolveSampleMapping(
      xenos::MsaaSamples::k2X, xenos::MsaaSamples::k2X, xenos::MsaaSamples::k2X,
      xenos::CopySampleSelect::k01);
  CHECK(fallback.content_samples == xenos::MsaaSamples::k2X);
  CHECK(fallback.requested_samples == xenos::MsaaSamples::k2X);
  CHECK(fallback.sample_select == xenos::CopySampleSelect::k01);
  CHECK_FALSE(fallback.expanded_to_physical_samples);
}

TEST_CASE("GTA IV native placement materialization uses a direct physical copy only when safe") {
  CHECK(CanDirectlyMaterializePhysicalSamples(true, 4u, 4u));
  CHECK(CanDirectlyMaterializePhysicalSamples(true, 2u, 2u));
  CHECK_FALSE(CanDirectlyMaterializePhysicalSamples(false, 4u, 4u));
  CHECK_FALSE(CanDirectlyMaterializePhysicalSamples(true, 2u, 4u));
}

TEST_CASE("GTA IV unified anti-aliasing exposes only canonical public values") {
  CHECK(ParseAntiAliasingMode("off") == AntiAliasingMode::kOff);
  CHECK(ParseAntiAliasingMode("fxaa") == AntiAliasingMode::kFxaa);
  CHECK(ParseAntiAliasingMode("smaa") == AntiAliasingMode::kSmaa);
  CHECK(ParseAntiAliasingMode("msaa2x") == AntiAliasingMode::kMsaa2x);
  CHECK(ParseAntiAliasingMode("msaa4x") == AntiAliasingMode::kMsaa4x);
  CHECK(ParseAntiAliasingMode("ssaa2x") == AntiAliasingMode::kSsaa2x);
  CHECK(ParseAntiAliasingMode("ssaa4x") == AntiAliasingMode::kSsaa4x);
  CHECK(ParseAntiAliasingMode("ssaa6x") == AntiAliasingMode::kSsaa6x);
  CHECK(ParseAntiAliasingMode("ssaa8x") == AntiAliasingMode::kSsaa8x);
  CHECK(ParseAntiAliasingMode("ssaa10x") == AntiAliasingMode::kSsaa10x);
  CHECK(ParseAntiAliasingMode("ssaa12x") == AntiAliasingMode::kSsaa12x);
  CHECK(ParseAntiAliasingMode("ssaa14x") == AntiAliasingMode::kSsaa14x);
  CHECK(ParseAntiAliasingMode("ssaa16x") == AntiAliasingMode::kSsaa16x);

  CHECK_FALSE(ParseAntiAliasingMode("spatial").has_value());
  CHECK_FALSE(ParseAntiAliasingMode("2x").has_value());
  CHECK_FALSE(ParseAntiAliasingMode("4x").has_value());
}

TEST_CASE("GTA IV unified anti-aliasing resolves legacy configurations deterministically") {
  const auto fxaa_over_msaa = ResolveAntiAliasingConfiguration("fxaa", "4x", true);
  CHECK(fxaa_over_msaa.mode == AntiAliasingMode::kFxaa);
  CHECK(fxaa_over_msaa.ignored_legacy_scene_msaa);

  const auto smaa_over_msaa = ResolveAntiAliasingConfiguration("smaa", "2x", true);
  CHECK(smaa_over_msaa.mode == AntiAliasingMode::kSmaa);
  CHECK(smaa_over_msaa.ignored_legacy_scene_msaa);

  CHECK(ResolveAntiAliasingConfiguration("off", "2x", false).mode ==
        AntiAliasingMode::kMsaa2x);
  CHECK(ResolveAntiAliasingConfiguration("off", "4x", false).mode ==
        AntiAliasingMode::kMsaa4x);
  CHECK(ResolveAntiAliasingConfiguration("off", "4x", false, true).mode ==
        AntiAliasingMode::kOff);
  CHECK(ResolveAntiAliasingConfiguration("off", "original", false).mode ==
        AntiAliasingMode::kOff);
  CHECK(ResolveAntiAliasingConfiguration("spatial", "original", false).mode ==
        AntiAliasingMode::kFxaa);
  CHECK(ResolveAntiAliasingConfiguration("invalid", "original", true).mode ==
        AntiAliasingMode::kFxaa);
  CHECK(ResolveAntiAliasingConfiguration("invalid", "original", false).mode ==
        AntiAliasingMode::kOff);
}

TEST_CASE("GTA IV unified anti-aliasing routes exactly one implementation") {
  for (AntiAliasingMode mode : {AntiAliasingMode::kOff, AntiAliasingMode::kFxaa,
                                AntiAliasingMode::kSmaa, AntiAliasingMode::kMsaa2x,
                                AntiAliasingMode::kMsaa4x, AntiAliasingMode::kSsaa2x,
                                AntiAliasingMode::kSsaa4x, AntiAliasingMode::kSsaa6x,
                                AntiAliasingMode::kSsaa8x, AntiAliasingMode::kSsaa10x,
                                AntiAliasingMode::kSsaa12x, AntiAliasingMode::kSsaa14x,
                                AntiAliasingMode::kSsaa16x}) {
    CHECK(HasExclusiveAntiAliasingRoute(GetAntiAliasingRoute(mode)));
  }

  const auto off = GetAntiAliasingRoute(AntiAliasingMode::kOff);
  CHECK_FALSE(off.presentation_fxaa);
  CHECK_FALSE(off.presentation_smaa);
  CHECK(off.scene_sample_count == 1u);

  const auto fxaa = GetAntiAliasingRoute(AntiAliasingMode::kFxaa);
  CHECK(fxaa.presentation_fxaa);
  CHECK_FALSE(fxaa.presentation_smaa);
  CHECK(fxaa.scene_sample_count == 1u);

  const auto smaa = GetAntiAliasingRoute(AntiAliasingMode::kSmaa);
  CHECK_FALSE(smaa.presentation_fxaa);
  CHECK(smaa.presentation_smaa);
  CHECK(smaa.scene_sample_count == 1u);

  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kMsaa2x).scene_sample_count == 2u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kMsaa4x).scene_sample_count == 4u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa2x).supersampling_pixel_factor == 2u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa4x).supersampling_pixel_factor == 4u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa6x).supersampling_pixel_factor == 6u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa8x).supersampling_pixel_factor == 8u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa10x).supersampling_pixel_factor == 10u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa12x).supersampling_pixel_factor == 12u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa14x).supersampling_pixel_factor == 14u);
  CHECK(GetAntiAliasingRoute(AntiAliasingMode::kSsaa16x).supersampling_pixel_factor == 16u);
}

TEST_CASE("GTA IV unified anti-aliasing applies only topology-compatible changes live") {
  CHECK(CanApplyAntiAliasingLive(AntiAliasingMode::kOff, AntiAliasingMode::kFxaa));
  CHECK(CanApplyAntiAliasingLive(AntiAliasingMode::kFxaa, AntiAliasingMode::kSmaa));
  CHECK(CanApplyAntiAliasingLive(AntiAliasingMode::kMsaa2x, AntiAliasingMode::kMsaa2x));

  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kSmaa,
                                      AntiAliasingMode::kMsaa2x));
  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kMsaa4x,
                                      AntiAliasingMode::kOff));
  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kMsaa2x,
                                      AntiAliasingMode::kMsaa4x));
  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kSmaa,
                                      AntiAliasingMode::kSsaa2x));
  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kSsaa2x,
                                      AntiAliasingMode::kSsaa4x));
  CHECK_FALSE(CanApplyAntiAliasingLive(AntiAliasingMode::kSsaa16x,
                                      AntiAliasingMode::kOff));
}

TEST_CASE("GTA IV SSAA accepts only even total pixel factors") {
  CHECK_FALSE(IsSupportedSupersamplingPixelFactor(1u));
  CHECK(IsSupportedSupersamplingPixelFactor(2u));
  CHECK_FALSE(IsSupportedSupersamplingPixelFactor(3u));
  CHECK(IsSupportedSupersamplingPixelFactor(16u));
  CHECK_FALSE(IsSupportedSupersamplingPixelFactor(18u));
}

TEST_CASE("GTA IV SSAA maps physical fragment coordinates back to the guest grid") {
  constexpr FragmentCoordinateScale kIdentity{1.0f, 1.0f};
  constexpr FragmentCoordinateScale kHalfScale{0.5f, 0.5f};
  CHECK(CalculateFragmentCoordinateScale(1920u, 1080u, 1920u, 1080u) == kIdentity);
  CHECK(CalculateFragmentCoordinateScale(1920u, 1080u, 3840u, 2160u) == kHalfScale);
  CHECK(CalculateFragmentCoordinateScale(0u, 1080u, 3840u, 2160u) == kIdentity);
}

TEST_CASE("GTA IV SSAA derives host-only even physical extents") {
  constexpr SupersampledExtent k1080p2x{2716u, 1528u};
  constexpr SupersampledExtent k1080p6x{4704u, 2646u};
  constexpr SupersampledExtent k4k16x{15360u, 8640u};
  CHECK(CalculateSupersampledExtent(1920u, 1080u, 2u) ==
        k1080p2x);
  CHECK(CalculateSupersampledExtent(1920u, 1080u, 6u) ==
        k1080p6x);
  CHECK(CalculateSupersampledExtent(3840u, 2160u, 16u) ==
        k4k16x);

  CHECK_FALSE(CalculateSupersampledExtent(1920u, 1080u, 6u, 4095u).has_value());
  CHECK_FALSE(CalculateSupersampledExtent(0u, 1080u, 2u).has_value());
  CHECK_FALSE(CalculateSupersampledExtent(1920u, 1080u, 3u).has_value());
}

}  // namespace
}  // namespace rex::graphics::gta4_native
