#pragma once

#include <rex/graphics/xenos.h>

namespace rex::graphics::vulkan {

// An enabled NEVER comparison discards fragments and must retain alpha tests.
// When testing is disabled, the stored comparison function is irrelevant.
constexpr bool CanOmitHostAlphaCoverage(bool alpha_test_enabled,
                                       xenos::CompareFunction alpha_function,
                                       bool alpha_to_mask_enabled) {
  return !alpha_to_mask_enabled &&
         (!alpha_test_enabled || alpha_function == xenos::CompareFunction::kAlways);
}

}  // namespace rex::graphics::vulkan
