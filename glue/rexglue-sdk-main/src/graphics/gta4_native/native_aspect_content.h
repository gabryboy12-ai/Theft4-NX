#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace rex::graphics::gta4_native {

struct NativeAspectContent {
  bool initialized = false;
  uint64_t writer_serial = 0;
  uint64_t ancestor_serial = 0;

  constexpr bool operator==(const NativeAspectContent&) const = default;
};

struct NativeSurfaceAspectContent {
  NativeAspectContent color{};
  NativeAspectContent depth{};
  NativeAspectContent stencil{};

  constexpr bool Has(VkImageAspectFlags aspects) const {
    constexpr VkImageAspectFlags supported = VK_IMAGE_ASPECT_COLOR_BIT |
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return aspects != 0 && !(aspects & ~supported) &&
           (!(aspects & VK_IMAGE_ASPECT_COLOR_BIT) || color.initialized) &&
           (!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT) || depth.initialized) &&
           (!(aspects & VK_IMAGE_ASPECT_STENCIL_BIT) || stencil.initialized);
  }

  constexpr void Write(VkImageAspectFlags aspects, uint64_t serial,
                       uint64_t source_serial = 0) {
    const auto write = [serial, source_serial](NativeAspectContent& content) {
      content.ancestor_serial = source_serial ? source_serial : content.writer_serial;
      content.writer_serial = serial;
      content.initialized = true;
    };
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) { write(color); }
    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) { write(depth); }
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) { write(stencil); }
  }

  constexpr void CopyFrom(const NativeSurfaceAspectContent& source,
                          VkImageAspectFlags aspects, uint64_t serial) {
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      Write(VK_IMAGE_ASPECT_COLOR_BIT, serial, source.color.writer_serial);
    }
    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      Write(VK_IMAGE_ASPECT_DEPTH_BIT, serial, source.depth.writer_serial);
    }
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      Write(VK_IMAGE_ASPECT_STENCIL_BIT, serial, source.stencil.writer_serial);
    }
  }
};

struct NativeAspectClearPlan {
  bool permitted = false;
  VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  bool explicit_clear = true;
};

constexpr NativeAspectClearPlan PlanNativeAspectClear(const NativeAspectContent& aspect,
                                                      bool current_placement,
                                                      bool full_rectangle,
                                                      bool prefer_load_clear = false) {
  // A whole attachment clear at render-pass begin replaces every old sample.
  // Partial clears still need the prior contents (or first-use initialization)
  // outside their rectangle followed by an explicit region clear.
  if (prefer_load_clear && full_rectangle) {
    return {true, VK_ATTACHMENT_LOAD_OP_CLEAR, false};
  }
  if (aspect.initialized && current_placement) {
    return {true, VK_ATTACHMENT_LOAD_OP_LOAD};
  }
  if (full_rectangle) {
    return {true, VK_ATTACHMENT_LOAD_OP_DONT_CARE};
  }
  if (!aspect.initialized) {
    return {true, VK_ATTACHMENT_LOAD_OP_CLEAR};
  }
  return {};
}

constexpr bool NativeAspectLineageUnchanged(const NativeAspectContent& setup,
                                            const NativeAspectContent& contribution) {
  return setup.initialized && contribution.initialized &&
         setup.writer_serial == contribution.writer_serial;
}

struct NativeLightingAttachmentLineage {
  uint64_t lifetime = 0;
  uint64_t image = 0;
  uint64_t view = 0;
  uint32_t samples = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  NativeAspectContent depth{};
  NativeAspectContent stencil{};
};

constexpr bool NativeLightingAttachmentUnchanged(const NativeLightingAttachmentLineage& setup,
                                                 const NativeLightingAttachmentLineage& draw) {
  return setup.image && setup.image == draw.image && setup.view == draw.view &&
         setup.lifetime == draw.lifetime && setup.samples == draw.samples &&
         setup.format == draw.format && setup.width == draw.width && setup.height == draw.height &&
         NativeAspectLineageUnchanged(setup.depth, draw.depth) &&
         NativeAspectLineageUnchanged(setup.stencil, draw.stencil);
}

}  // namespace rex::graphics::gta4_native
