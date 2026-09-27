#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/texture_filtering_policy.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("GTA IV bilinear filtering interpolates texels but not mip levels") {
  const MaterialTextureFilterState title_state{
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kLinear,
  };

  const MaterialTextureFilterState filtered =
      ApplyMaterialTextureFiltering(title_state, "bilinear", true);

  CHECK(filtered.min_filter == xenos::TextureFilter::kLinear);
  CHECK(filtered.mag_filter == xenos::TextureFilter::kLinear);
  CHECK(filtered.mip_filter == xenos::TextureFilter::kPoint);
}

TEST_CASE("GTA IV trilinear filtering interpolates texels and mip levels") {
  const MaterialTextureFilterState title_state{
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kPoint,
  };

  const MaterialTextureFilterState filtered =
      ApplyMaterialTextureFiltering(title_state, "trilinear", true);

  CHECK(filtered.min_filter == xenos::TextureFilter::kLinear);
  CHECK(filtered.mag_filter == xenos::TextureFilter::kLinear);
  CHECK(filtered.mip_filter == xenos::TextureFilter::kLinear);
}

TEST_CASE("GTA IV texture filtering preserves title state outside material textures") {
  const MaterialTextureFilterState title_state{
      xenos::TextureFilter::kPoint,
      xenos::TextureFilter::kLinear,
      xenos::TextureFilter::kBaseMap,
  };

  CHECK(ApplyMaterialTextureFiltering(title_state, "bilinear", false) == title_state);
  CHECK(ApplyMaterialTextureFiltering(title_state, "trilinear", false) == title_state);
  CHECK(ApplyMaterialTextureFiltering(title_state, "original", true) == title_state);
  CHECK(ApplyMaterialTextureFiltering(title_state, "invalid", true) == title_state);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
