#pragma once

#include <string_view>

#include <rex/graphics/xenos.h>

namespace rex::graphics::gta4_native {

struct MaterialTextureFilterState {
  xenos::TextureFilter min_filter = xenos::TextureFilter::kPoint;
  xenos::TextureFilter mag_filter = xenos::TextureFilter::kPoint;
  xenos::TextureFilter mip_filter = xenos::TextureFilter::kPoint;

  bool operator==(const MaterialTextureFilterState&) const = default;
};

// Bilinear and trilinear filtering differ only in how mip levels are selected:
// both interpolate texels within a mip, while trilinear additionally
// interpolates between the two adjacent mip levels. Keep unsupported values
// and ineligible resources title-authored.
constexpr MaterialTextureFilterState ApplyMaterialTextureFiltering(
    MaterialTextureFilterState title_state, std::string_view override_value,
    bool material_filter_eligible) {
  if (!material_filter_eligible) {
    return title_state;
  }
  if (override_value == "bilinear") {
    title_state.min_filter = xenos::TextureFilter::kLinear;
    title_state.mag_filter = xenos::TextureFilter::kLinear;
    title_state.mip_filter = xenos::TextureFilter::kPoint;
  } else if (override_value == "trilinear") {
    title_state.min_filter = xenos::TextureFilter::kLinear;
    title_state.mag_filter = xenos::TextureFilter::kLinear;
    title_state.mip_filter = xenos::TextureFilter::kLinear;
  }
  return title_state;
}

}  // namespace rex::graphics::gta4_native
