#pragma once

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {

// True only for a live logical device created with maintenance8 enabled.
// Keeps optional ownership-copy capability outside VulkanDevice's ABI.
bool StencilCopyEnabled(VkDevice device);

}  // namespace rex::ui::vulkan
