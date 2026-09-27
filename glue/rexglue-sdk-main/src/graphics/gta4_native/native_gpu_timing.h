#pragma once

#include <rex/ui/vulkan/api.h>

#include "native_performance_samples.h"

namespace rex::graphics::gta4_native {

// Allocation-free bridge used by self-contained renderer passes. The graphics
// system owns the query pool and supplies the callback only while a capture is
// active; passes merely mark exclusive range boundaries.
struct NativeGpuTimingSink {
  using SwitchRangeCallback = void (*)(void* context, VkCommandBuffer command_buffer,
                                       performance::GpuRange range);

  void* context = nullptr;
  SwitchRangeCallback switch_range = nullptr;

  void Switch(VkCommandBuffer command_buffer, performance::GpuRange range) const {
    if (switch_range) {
      switch_range(context, command_buffer, range);
    }
  }
};

}  // namespace rex::graphics::gta4_native
