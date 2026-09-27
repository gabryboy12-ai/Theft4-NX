#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/gta4_native/shadow_distance_util.h>

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("GTA IV native shadow distance scales both initialized contexts") {
  const std::array<float, kNativeShadowContextCount> originals = {512.0f, 512.0f};
  std::array<float, kNativeShadowContextCount> scaled = {-1.0f, -1.0f};

  REQUIRE(CalculateNativeShadowRanges(originals, 2.5, scaled));
  CHECK(scaled[0] == 1280.0f);
  CHECK(scaled[1] == 1280.0f);
}

TEST_CASE("GTA IV native shadow distance always derives from immutable originals") {
  const std::array<float, kNativeShadowContextCount> originals = {512.0f, 768.0f};
  std::array<float, kNativeShadowContextCount> first{};
  std::array<float, kNativeShadowContextCount> second{};

  REQUIRE(CalculateNativeShadowRanges(originals, 2.0, first));
  REQUIRE(CalculateNativeShadowRanges(originals, 3.0, second));
  CHECK(first[0] == 1024.0f);
  CHECK(first[1] == 1536.0f);
  CHECK(second[0] == 1536.0f);
  CHECK(second[1] == 2304.0f);
}

TEST_CASE("GTA IV native shadow distance rejects invalid input atomically") {
  std::array<float, kNativeShadowContextCount> output = {11.0f, 22.0f};
  const std::array<float, kNativeShadowContextCount> invalid_originals = {512.0f, 0.0f};

  CHECK_FALSE(CalculateNativeShadowRanges(invalid_originals, 2.0, output));
  CHECK(output[0] == 11.0f);
  CHECK(output[1] == 22.0f);

  const std::array<float, kNativeShadowContextCount> originals = {512.0f, 512.0f};
  CHECK_FALSE(CalculateNativeShadowRanges(
      originals, std::numeric_limits<double>::infinity(), output));
  CHECK(output[0] == 11.0f);
  CHECK(output[1] == 22.0f);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
