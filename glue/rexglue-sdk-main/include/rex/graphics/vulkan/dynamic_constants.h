/**
 * Copyright 2026. Released under the BSD license - see LICENSE.
 */
#ifndef REX_GRAPHICS_VULKAN_DYNAMIC_CONSTANTS_H_
#define REX_GRAPHICS_VULKAN_DYNAMIC_CONSTANTS_H_

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace rex::graphics::vulkan {

constexpr bool CanUseDynamicUniformConstants(bool requested,
                                             uint32_t dynamic_uniform_limit,
                                             uint32_t constant_buffer_count) {
  return requested && constant_buffer_count != 0 &&
         dynamic_uniform_limit >= constant_buffer_count;
}

constexpr VkDescriptorType GuestConstantDescriptorType(bool dynamic) {
  return dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// Keep existing descriptor offsets; dynamic offsets are all zero. Supply
// them exactly when the constants set is part of this bind range, including
// unused pixel bindings in the layout.
constexpr uint32_t GuestConstantDynamicOffsetCount(
    bool dynamic, uint32_t first_set, uint32_t set_count,
    uint32_t constant_set, uint32_t constant_buffer_count) {
  return dynamic && first_set <= constant_set &&
                 constant_set - first_set < set_count
             ? constant_buffer_count
             : 0;
}

}  // namespace rex::graphics::vulkan

#endif  // REX_GRAPHICS_VULKAN_DYNAMIC_CONSTANTS_H_
