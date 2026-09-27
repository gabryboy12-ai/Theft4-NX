#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "graphics/gta4_native/native_aspect_content.h"

namespace gta4 = rex::graphics::gta4_native;

TEST_CASE("Partial resolve clears preserve valid pixels or initialize only first-use contents") {
  const gta4::NativeAspectContent written{true, 1, 0};
  const auto current = gta4::PlanNativeAspectClear(written, true, false);
  REQUIRE(current.permitted);
  REQUIRE(current.load == VK_ATTACHMENT_LOAD_OP_LOAD);
  const auto undefined = gta4::PlanNativeAspectClear({}, false, false);
  REQUIRE(undefined.permitted);
  REQUIRE(undefined.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
  REQUIRE_FALSE(gta4::PlanNativeAspectClear(written, false, false).permitted);
}

TEST_CASE("Full resolve clears can replace stale contents without reading them") {
  const gta4::NativeAspectContent written{true, 1, 0};
  const auto stale = gta4::PlanNativeAspectClear(written, false, true);
  REQUIRE(stale.permitted);
  REQUIRE(stale.load == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  const auto undefined = gta4::PlanNativeAspectClear({}, false, true);
  REQUIRE(undefined.permitted);
  REQUIRE(undefined.load == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  const auto current = gta4::PlanNativeAspectClear(written, true, true);
  REQUIRE(current.permitted);
  REQUIRE(current.load == VK_ATTACHMENT_LOAD_OP_LOAD);
}

TEST_CASE("Partial combined clear preserves stencil when only depth is undefined") {
  gta4::NativeSurfaceAspectContent content{};
  content.Write(VK_IMAGE_ASPECT_STENCIL_BIT, 1);
  const auto depth = gta4::PlanNativeAspectClear(content.depth, true, false);
  const auto stencil = gta4::PlanNativeAspectClear(content.stencil, true, false);
  REQUIRE(depth.permitted);
  REQUIRE(depth.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
  REQUIRE(stencil.permitted);
  REQUIRE(stencil.load == VK_ATTACHMENT_LOAD_OP_LOAD);

  // Losing placement ownership must not turn the defined stencil into a
  // first-use plane just because depth has never been initialized.
  REQUIRE(gta4::PlanNativeAspectClear(content.depth, false, false).permitted);
  REQUIRE_FALSE(gta4::PlanNativeAspectClear(content.stencil, false, false).permitted);
}

TEST_CASE("Load clears replace full resolve contents regardless of prior ownership") {
  for (bool initialized : {false, true}) {
    for (bool current : {false, true}) {
      const gta4::NativeAspectContent content{initialized, 1, 0};
      const auto full = gta4::PlanNativeAspectClear(content, current, true, true);
      REQUIRE(full.permitted);
      REQUIRE(full.load == VK_ATTACHMENT_LOAD_OP_CLEAR);
      REQUIRE_FALSE(full.explicit_clear);

      // Disabling the optimization retains the old path, including LOAD when
      // current contents exist and an explicit clear in every permitted case.
      const auto fallback = gta4::PlanNativeAspectClear(content, current, true, false);
      REQUIRE(fallback.permitted);
      REQUIRE(fallback.explicit_clear);
      REQUIRE(fallback.load == (initialized && current ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                       : VK_ATTACHMENT_LOAD_OP_DONT_CARE));

      const auto partial = gta4::PlanNativeAspectClear(content, current, false, true);
      const auto partial_fallback = gta4::PlanNativeAspectClear(content, current, false, false);
      REQUIRE(partial.permitted == partial_fallback.permitted);
      REQUIRE(partial.load == partial_fallback.load);
      REQUIRE(partial.explicit_clear);
    }
  }
}

TEST_CASE("Resolve load clears preserve per-aspect contents and every sample") {
  // Model a three by two attachment with distinct samples. The partial clear
  // selects the middle column; its surrounding pixels must retain their own
  // aspect contents or the first-use initialization value. Values stand for
  // distinct color/depth/stencil payloads after their format conversion.
  using Plane = std::array<std::array<uint32_t, 4>, 6>;
  const Plane original{{{11, 12, 13, 14}, {21, 22, 23, 24}, {31, 32, 33, 34},
                        {41, 42, 43, 44}, {51, 52, 53, 54}, {61, 62, 63, 64}}};
  const std::array<uint32_t, 3> requested_values{0xA1234567, 0x3EC00000, 0xA5};
  const std::array<uint32_t, 3> initial_values{0xFF000000, 0x3F800000, 0};
  for (size_t aspect = 0; aspect < requested_values.size(); ++aspect) {
    for (bool initialized : {false, true}) {
      for (bool current : {false, true}) {
        for (bool full : {false, true}) {
          const gta4::NativeAspectContent content{initialized, 1, 0};
          const auto baseline = gta4::PlanNativeAspectClear(content, current, full, false);
          const auto optimized = gta4::PlanNativeAspectClear(content, current, full, true);
          REQUIRE(baseline.permitted == optimized.permitted);
          if (!baseline.permitted) {
            continue;
          }
          const auto execute = [&](const gta4::NativeAspectClearPlan& plan) {
            Plane result = original;
            if (plan.load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
              for (auto& pixel : result) {
                pixel.fill(plan.explicit_clear ? initial_values[aspect] : requested_values[aspect]);
              }
            }
            if (plan.explicit_clear) {
              for (size_t pixel = 0; pixel < result.size(); ++pixel) {
                if (full || pixel == 1 || pixel == 4) {
                  result[pixel].fill(requested_values[aspect]);
                }
              }
            }
            return result;
          };
          const Plane actual = execute(optimized);
          REQUIRE(actual == execute(baseline));
          for (size_t pixel = 0; pixel < actual.size(); ++pixel) {
            const bool cleared = full || pixel == 1 || pixel == 4;
            for (size_t sample = 0; sample < actual[pixel].size(); ++sample) {
              REQUIRE(actual[pixel][sample] ==
                      (cleared ? requested_values[aspect]
                               : initialized ? original[pixel][sample] : initial_values[aspect]));
            }
          }
        }
      }
    }
  }
}
