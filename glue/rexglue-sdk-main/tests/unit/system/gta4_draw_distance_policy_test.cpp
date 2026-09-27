#include <bit>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "../../../gta4-recomp/src/gta4_draw_distance_policy.h"

namespace draw_distance = gta4::draw_distance;

TEST_CASE("GTA IV draw-distance policy publishes configured engine scales",
          "[gta4][draw-distance]") {
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(1.0)) == 0x3F800000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(3.0)) == 0x40400000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(4.0)) == 0x40800000);
}

TEST_CASE("GTA IV draw-distance policy rejects invalid host scales",
          "[gta4][draw-distance]") {
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(0.0)) == 0x3F800000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(-1.0)) == 0x3F800000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(
            std::numeric_limits<double>::infinity())) == 0x3F800000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(
            std::numeric_limits<double>::quiet_NaN())) == 0x3F800000);
  CHECK(std::bit_cast<uint32_t>(draw_distance::ResolveEngineScale(
            std::numeric_limits<double>::max())) == 0x3F800000);
}
