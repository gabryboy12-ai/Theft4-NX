#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/graphics_upload_buffer_pool.h>
#include <rex/ui/vulkan/device.h>
#include <vector>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanUploadBufferPool : public GraphicsUploadBufferPool {
 public:
  VulkanUploadBufferPool(const VulkanDevice* vulkan_device, VkBufferUsageFlags usage,
                         size_t page_size = kDefaultPageSize,
                         bool device_local_mirror = false);

  struct DeviceUploadStats {
    uint64_t copies = 0;
    uint64_t bytes = 0;
  };
  // Call after FlushWrites, outside any render pass, before commands using
  // returned buffers. CPU mappings remain staging-only; mirrors are never
  // mapped, so GPU caching does not break host coherence on the Switch.
  DeviceUploadStats RecordDeviceUploads(VkCommandBuffer command_buffer,
                                       VkPipelineStageFlags consumer_stages);
  void ClearCache();
  // Every page, including an optional device mirror, has this buffer size.
  // The first successful allocation fixes any allocation-padding expansion.
  VkDeviceSize GetPageSizeForBindings() const { return VkDeviceSize(page_size_); }

  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment, VkBuffer& buffer_out,
                   VkDeviceSize& offset_out);
  uint8_t* RequestPartial(uint64_t submission_index, size_t size, size_t alignment,
                          VkBuffer& buffer_out, VkDeviceSize& offset_out, VkDeviceSize& size_out);

 protected:
  Page* CreatePageImplementation() override;

  void FlushPageWrites(Page* page, size_t offset, size_t size) override;

 private:
  struct VulkanPage : public Page {
    // Takes ownership of the buffer and its memory and mapping.
    VulkanPage(const VulkanDevice* vulkan_device, VkBuffer buffer, VkDeviceMemory memory,
               void* mapping, VkBuffer device_buffer, VkDeviceMemory device_memory)
        : vulkan_device_(vulkan_device), buffer_(buffer), memory_(memory), mapping_(mapping),
          device_buffer_(device_buffer), device_memory_(device_memory) {}
    ~VulkanPage() override;
    const VulkanDevice* vulkan_device_;
    VkBuffer buffer_;
    VkDeviceMemory memory_;
    void* mapping_;
    VkBuffer device_buffer_;
    VkDeviceMemory device_memory_;
  };

  const VulkanDevice* vulkan_device_;

  VkDeviceSize allocation_size_;
  static constexpr uint32_t kMemoryTypeUnknown = UINT32_MAX;
  static constexpr uint32_t kMemoryTypeUnavailable = kMemoryTypeUnknown - 1;
  uint32_t memory_type_ = UINT32_MAX;

  VkBufferUsageFlags usage_;
  bool device_local_mirror_;
  struct PendingDeviceUpload {
    VkBuffer source;
    VkBuffer destination;
    VkBufferCopy region;
  };
  std::vector<PendingDeviceUpload> pending_device_uploads_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
