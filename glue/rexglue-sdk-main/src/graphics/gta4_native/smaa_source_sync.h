#pragma once

#include <rex/ui/vulkan/api.h>

namespace rex::graphics::gta4_native {

struct SmaaSourceSynchronization {
  bool needs_barrier = true;
  VkAccessFlags access = 0;
  VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
};

// Native producers publish shader-read layouts with fragment visibility. SMAA
// may reuse that publication; other layouts need their producer/read scope
// completed before changing the layout for fragment sampling.
inline SmaaSourceSynchronization GetSmaaSourceSynchronization(VkImageLayout layout) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return {false, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return {true, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {true, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {true, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    default:
      // GENERAL can contain arbitrary writes; narrow it only after the caller
      // carries explicit last-writer information rather than guessing.
      return {true, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  }
}

}  // namespace rex::graphics::gta4_native
