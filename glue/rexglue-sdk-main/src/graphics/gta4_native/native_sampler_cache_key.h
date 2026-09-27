#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include <xxhash.h>
#include <rex/ui/vulkan/api.h>

namespace rex::graphics::gta4_native {

// One native device owns four UI samplers, the null sampler, two SMAA
// samplers, and one sampler each for split post-processing and sun shafts.
inline constexpr uint32_t kNativeAuxiliarySamplerCount = 9;

inline bool CanAllocateNativeCachedSampler(size_t cached_count, uint32_t device_limit) {
  return device_limit > kNativeAuxiliarySamplerCount &&
         cached_count < device_limit - kNativeAuxiliarySamplerCount;
}

// The native renderer creates normalized-coordinate, non-comparison samplers
// without extension structures. Store only their effective Vulkan state, with
// no structure padding, pointers, or ignored fields in the cache identity.
struct NativeSamplerCacheKey {
  std::array<uint32_t, 12> words{};

  bool operator==(const NativeSamplerCacheKey&) const = default;

  static NativeSamplerCacheKey FromCreateInfo(const VkSamplerCreateInfo& info) {
    const auto float_bits = [](float value) {
      return std::bit_cast<uint32_t>(value == 0.0f ? 0.0f : value);
    };
    const bool uses_border = info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             info.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    return {{uint32_t(info.minFilter), uint32_t(info.magFilter), uint32_t(info.mipmapMode),
             uint32_t(info.addressModeU), uint32_t(info.addressModeV), uint32_t(info.addressModeW),
             float_bits(info.mipLodBias), uint32_t(info.anisotropyEnable != VK_FALSE),
             info.anisotropyEnable ? float_bits(info.maxAnisotropy) : 0,
             float_bits(info.minLod), float_bits(info.maxLod),
             uses_border ? uint32_t(info.borderColor) : 0}};
  }
};

struct NativeSamplerCacheKeyHash {
  size_t operator()(const NativeSamplerCacheKey& key) const {
    return size_t(XXH3_64bits(key.words.data(), sizeof(key.words)));
  }
};

}  // namespace rex::graphics::gta4_native
