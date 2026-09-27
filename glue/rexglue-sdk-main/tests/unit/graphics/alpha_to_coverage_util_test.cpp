/**
 * @file alpha_to_coverage_util_test.cpp
 * @brief GTA IV native-renderer alpha-to-mask policy tests.
 */

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/alpha_to_coverage_util.h"

namespace rex::graphics::gta4_native {

TEST_CASE("GTA IV native alpha-to-mask preserves the Xenos offset byte",
          "[gta4-native][graphics][alpha-to-coverage]") {
  CHECK(PackNativeAlphaToMask(0xAA000010u) == 0x000001AAu);
  CHECK(PackNativeAlphaToMask(0x6C000010u) == 0x0000016Cu);
  CHECK(PackNativeAlphaToMask(0xAA000000u) == 0u);

  CHECK(IsNativeAlphaToMaskRequested(0x000001AAu));
  CHECK_FALSE(IsNativeAlphaToMaskRequested(0x000000AAu));
  CHECK(IsNativeFragmentCoverageRequested(true, 0u));
  CHECK(IsNativeFragmentCoverageRequested(false, 0x000001AAu));
  CHECK_FALSE(IsNativeFragmentCoverageRequested(false, 0u));
}

}  // namespace rex::graphics::gta4_native
