#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

#include "native_fixed_function_policy.h"

namespace rex::graphics::gta4_native {

// Calculated from guest state before images are allocated. Inactive bound
// descriptors cannot constrain dimensions, sample count, or target families.
struct NativeAttachmentUsage {
  uint32_t color_attachment_mask = 0;
  uint32_t color_write_mask = 0;
  VkImageAspectFlags depth_stencil_aspects = 0;
};

constexpr NativeAttachmentUsage SelectNativeDrawAttachmentUsage(
    uint32_t color_write_mask, uint32_t shader_output_mask,
    uint32_t format_component_masks, bool depth_enable, bool stencil_enable) {
  NativeAttachmentUsage usage{};
  usage.color_write_mask = NormalizeNativeColorWriteMask(
      color_write_mask, shader_output_mask, format_component_masks);
  usage.color_attachment_mask = NativeColorTargetMaskFromWriteMask(usage.color_write_mask);
  if (depth_enable) {
    usage.depth_stencil_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
  }
  if (stencil_enable) {
    usage.depth_stencil_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  return usage;
}

constexpr NativeAttachmentUsage SelectNativeClearAttachmentUsage(uint32_t flags) {
  NativeAttachmentUsage usage{};
  usage.color_attachment_mask = flags & 0xFu;
  for (uint32_t index = 0; index < kNativeFixedRenderTargetCount; ++index) {
    if (usage.color_attachment_mask & (1u << index)) {
      usage.color_write_mask |= 0xFu << (index * 4u);
    }
  }
  if (flags & 0x10u) {
    usage.depth_stencil_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
  }
  if (flags & 0x20u) {
    usage.depth_stencil_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  return usage;
}

// No tracing fields enter attachment selection. Depth and stencil use separate
// VkRenderingAttachmentInfo objects so their load operations stay independent.
inline void SetNativeRenderingAttachmentBindings(
    VkRenderingInfo& rendering, VkImageAspectFlags aspects,
    const VkRenderingAttachmentInfo& depth_or_color,
    const VkRenderingAttachmentInfo& stencil) {
  rendering.colorAttachmentCount = (aspects & VK_IMAGE_ASPECT_COLOR_BIT) ? 1u : 0u;
  rendering.pColorAttachments = rendering.colorAttachmentCount ? &depth_or_color : nullptr;
  rendering.pDepthAttachment = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &depth_or_color : nullptr;
  rendering.pStencilAttachment = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &stencil : nullptr;
}

}  // namespace rex::graphics::gta4_native
