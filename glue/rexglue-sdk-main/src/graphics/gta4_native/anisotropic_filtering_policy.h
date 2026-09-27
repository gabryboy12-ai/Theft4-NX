#pragma once

#include <string_view>

#include <rex/graphics/xenos.h>

namespace rex::graphics::gta4_native {

struct MaterialAnisotropicFilterState {
  xenos::TextureFilter min_filter = xenos::TextureFilter::kPoint;
  xenos::TextureFilter mag_filter = xenos::TextureFilter::kPoint;
  xenos::TextureFilter mip_filter = xenos::TextureFilter::kPoint;
  xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kDisabled;

  bool operator==(const MaterialAnisotropicFilterState&) const = default;
};

// Keep non-material and unknown settings title-authored. Treat 1x as disabled
// anisotropy so the independent texture-filtering selection remains intact;
// enabling anisotropy with a 1.0 clamp has historically varied across Vulkan
// implementations. Higher ratios use linear sampling consistently.
constexpr MaterialAnisotropicFilterState ApplyMaterialAnisotropicFiltering(
    MaterialAnisotropicFilterState title_state, std::string_view override_value,
    bool material_filter_eligible) {
  if (!material_filter_eligible) {
    return title_state;
  }

  xenos::AnisoFilter override_filter;
  if (override_value == "1x") {
    title_state.aniso_filter = xenos::AnisoFilter::kDisabled;
    return title_state;
  } else if (override_value == "2x") {
    override_filter = xenos::AnisoFilter::kMax_2_1;
  } else if (override_value == "4x") {
    override_filter = xenos::AnisoFilter::kMax_4_1;
  } else if (override_value == "8x") {
    override_filter = xenos::AnisoFilter::kMax_8_1;
  } else if (override_value == "16x") {
    override_filter = xenos::AnisoFilter::kMax_16_1;
  } else {
    return title_state;
  }

  title_state.min_filter = xenos::TextureFilter::kLinear;
  title_state.mag_filter = xenos::TextureFilter::kLinear;
  title_state.mip_filter = xenos::TextureFilter::kLinear;
  title_state.aniso_filter = override_filter;
  return title_state;
}

}  // namespace rex::graphics::gta4_native
