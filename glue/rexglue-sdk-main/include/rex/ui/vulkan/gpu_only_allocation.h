/**
 ******************************************************************************
 * ReXGlue Runtime                                                            *
 ******************************************************************************
 * Copyright 2026. Released under the BSD license.                            *
 ******************************************************************************
 *
 * Marks Vulkan allocations made for memory the CPU never maps
 * (MemoryPurpose::kDeviceLocal buffers and images, texture images). NVK binds
 * an allocation's GPU address inside vkAllocateMemory and vkBindImageMemory,
 * on the calling thread, so a hook below the driver can read the mark there:
 * on the Switch, switch-game maps such memory GPU-cacheable (cvar nx_gpu_l2),
 * because NVK's port maps everything uncached, which is only needed where the
 * CPU reads or writes.
 */

#pragma once

namespace rex::ui::vulkan {

// The mark of the calling thread.
bool& GpuOnlyAllocation();

class GpuOnlyAllocationScope {
 public:
  explicit GpuOnlyAllocationScope(bool gpu_only) : previous_(GpuOnlyAllocation()) {
    GpuOnlyAllocation() = gpu_only;
  }
  ~GpuOnlyAllocationScope() { GpuOnlyAllocation() = previous_; }
  GpuOnlyAllocationScope(const GpuOnlyAllocationScope&) = delete;
  GpuOnlyAllocationScope& operator=(const GpuOnlyAllocationScope&) = delete;

 private:
  bool previous_;
};

}  // namespace rex::ui::vulkan
