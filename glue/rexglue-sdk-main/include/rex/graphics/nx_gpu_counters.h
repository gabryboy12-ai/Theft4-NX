// Counters of the Vulkan backend that switch-game prints in its heartbeat
// (docs/switch-port/06-game-boot-plan.md section 25, run 19). Plain relaxed
// atomics, read from the UI thread; nothing here allocates or locks.

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics {

struct NxGpuCounters {
  std::atomic<uint64_t> presents{0};          // vkQueuePresentKHR == VK_SUCCESS/SUBOPTIMAL
  std::atomic<uint64_t> present_failures{0};  // any other result
  std::atomic<int32_t> last_present_failure{0};
  std::atomic<uint64_t> submits{0};           // vkQueueSubmit of the command processor and presenter
  std::atomic<uint64_t> submit_failures{0};
  std::atomic<int32_t> last_submit_failure{0};
  std::atomic<uint64_t> draws{0};             // VulkanCommandProcessor::IssueDraw calls
};

inline NxGpuCounters& nx_gpu_counters() {
  static NxGpuCounters counters;
  return counters;
}

// One line per failure, the first 32 only (REXGPU_ERROR at the call site).
inline bool NxGpuCountSubmit(int32_t result) {
  auto& c = nx_gpu_counters();
  if (result == 0) {
    c.submits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  c.last_submit_failure.store(result, std::memory_order_relaxed);
  return c.submit_failures.fetch_add(1, std::memory_order_relaxed) < 32;
}

// VK_SUBOPTIMAL_KHR (1000001003) is a presented image.
inline bool NxGpuCountPresent(int32_t result) {
  auto& c = nx_gpu_counters();
  if (result == 0 || result == 1000001003) {
    c.presents.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  c.last_present_failure.store(result, std::memory_order_relaxed);
  return c.present_failures.fetch_add(1, std::memory_order_relaxed) < 32;
}

}  // namespace rex::graphics
