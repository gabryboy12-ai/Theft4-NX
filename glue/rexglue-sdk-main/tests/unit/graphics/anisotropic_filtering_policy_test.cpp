#include <array>

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/anisotropic_filtering_policy.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("GTA IV 1x anisotropic filtering disables anisotropy only") {
  const MaterialAnisotropicFilterState title_state{
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kLinear,
      xenos::TextureFilter::kPoint,
      xenos::AnisoFilter::kMax_16_1,
  };

  const MaterialAnisotropicFilterState filtered =
      ApplyMaterialAnisotropicFiltering(title_state, "1x", true);

  CHECK(filtered.min_filter == title_state.min_filter);
  CHECK(filtered.mag_filter == title_state.mag_filter);
  CHECK(filtered.mip_filter == title_state.mip_filter);
  CHECK(filtered.aniso_filter == xenos::AnisoFilter::kDisabled);
}

TEST_CASE("GTA IV anisotropic filtering maps enhanced menu levels exactly") {
  struct ExpectedMapping {
    std::string_view value;
    xenos::AnisoFilter filter;
  };
  constexpr std::array kMappings = {
      ExpectedMapping{"2x", xenos::AnisoFilter::kMax_2_1},
      ExpectedMapping{"4x", xenos::AnisoFilter::kMax_4_1},
      ExpectedMapping{"8x", xenos::AnisoFilter::kMax_8_1},
      ExpectedMapping{"16x", xenos::AnisoFilter::kMax_16_1},
  };

  for (const ExpectedMapping& mapping : kMappings) {
    const MaterialAnisotropicFilterState filtered = ApplyMaterialAnisotropicFiltering(
        {xenos::TextureFilter::kPoint, xenos::TextureFilter::kPoint, xenos::TextureFilter::kPoint,
         xenos::AnisoFilter::kDisabled},
        mapping.value, true);
    CHECK(filtered.min_filter == xenos::TextureFilter::kLinear);
    CHECK(filtered.mag_filter == xenos::TextureFilter::kLinear);
    CHECK(filtered.mip_filter == xenos::TextureFilter::kLinear);
    CHECK(filtered.aniso_filter == mapping.filter);
  }
}

TEST_CASE("GTA IV anisotropic filtering preserves ineligible title samplers") {
  const MaterialAnisotropicFilterState title_state{
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kLinear,
      xenos::TextureFilter::kBaseMap,
      xenos::AnisoFilter::kMax_2_1,
  };

  CHECK(ApplyMaterialAnisotropicFiltering(title_state, "16x", false) == title_state);
  CHECK(ApplyMaterialAnisotropicFiltering(title_state, "original", true) == title_state);
  CHECK(ApplyMaterialAnisotropicFiltering(title_state, "off", true) == title_state);
  CHECK(ApplyMaterialAnisotropicFiltering(title_state, "invalid", true) == title_state);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
