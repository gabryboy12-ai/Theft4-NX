#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::graphics::gta4_native {

// Xenos fetch word 4 stores a signed 10-bit value in units of 1/32.
// The shader applies it because portability devices may not support hardware
// sampler bias. Clamp before either implicit sampling or explicit LOD addition,
// matching Vulkan's LOD operation with a zero hardware sampler bias.
inline float NativeSamplerLodBias(int32_t fetch_bias, float device_limit) {
  return std::clamp(float(fetch_bias) * (1.0f / 32.0f), -device_limit, device_limit);
}

}  // namespace rex::graphics::gta4_native
