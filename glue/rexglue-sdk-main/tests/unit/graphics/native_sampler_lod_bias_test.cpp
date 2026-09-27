#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_sampler_lod_bias.h"

using rex::graphics::gta4_native::NativeSamplerLodBias;

TEST_CASE("Native sampler bias preserves signed fetch values and device limits", "[graphics][sampler]") {
  REQUIRE(NativeSamplerLodBias(0, 16.0f) == 0.0f);
  REQUIRE(NativeSamplerLodBias(1, 16.0f) == 0.03125f);
  REQUIRE(NativeSamplerLodBias(-1, 16.0f) == -0.03125f);
  REQUIRE(NativeSamplerLodBias(-512, 16.0f) == -16.0f);
  REQUIRE(NativeSamplerLodBias(511, 16.0f) == 15.96875f);
  REQUIRE(NativeSamplerLodBias(-512, 2.0f) == -2.0f);
  REQUIRE(NativeSamplerLodBias(511, 2.0f) == 2.0f);
  REQUIRE(NativeSamplerLodBias(511, 0.0f) == 0.0f);
}
