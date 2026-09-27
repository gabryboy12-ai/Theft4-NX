#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "../../../gta4-recomp/src/gta4_population_policy.h"

namespace population = gta4::population;

TEST_CASE("GTA IV population policy scales positive game requests",
          "[gta4][population]") {
  CHECK(std::bit_cast<uint32_t>(population::ScaleMultiplier(1.0f, 1.25)) ==
        0x3FA00000);
  CHECK(std::bit_cast<uint32_t>(population::ScaleMultiplier(0.5f, 1.25)) ==
        0x3F200000);
  CHECK(std::bit_cast<uint32_t>(population::ScaleMultiplier(2.0f, 1.25)) ==
        0x40200000);
}

TEST_CASE("GTA IV population policy preserves game suppression and sentinels",
          "[gta4][population]") {
  CHECK(std::bit_cast<uint32_t>(population::ScaleMultiplier(0.0f, 4.0)) ==
        0x00000000);
  CHECK(std::bit_cast<uint32_t>(population::ScaleMultiplier(-0.0f, 4.0)) ==
        0x80000000);
  CHECK(population::ScaleMultiplier(-1.0f, 4.0) == -1.0f);
  CHECK(std::isnan(population::ScaleMultiplier(
      std::numeric_limits<float>::quiet_NaN(), 4.0)));
  CHECK(population::ScaleMultiplier(
            -std::numeric_limits<float>::infinity(), 4.0) ==
        -std::numeric_limits<float>::infinity());
}

TEST_CASE("GTA IV population policy validates host scale and overflow",
          "[gta4][population]") {
  CHECK(population::ScaleMultiplier(1.0f, 0.0) == 0.0f);
  CHECK(population::ScaleMultiplier(1.0f, -1.0) == 1.0f);
  CHECK(population::ScaleMultiplier(
            1.0f, std::numeric_limits<double>::quiet_NaN()) == 1.0f);
  CHECK(population::ScaleMultiplier(
            std::numeric_limits<float>::max(), 4.0) ==
        std::numeric_limits<float>::max());
}
