#include <catch2/catch_test_macros.hpp>

#include <unordered_map>

#include "graphics/gta4_native/native_sampler_cache_key.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("Native sampler cache ignores Vulkan-inactive state and signed zero") {
  VkSamplerCreateInfo first{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  first.maxAnisotropy = 1.0f;
  first.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSamplerCreateInfo second = first;
  second.maxAnisotropy = 16.0f;
  second.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  second.mipLodBias = -0.0f;
  second.minLod = -0.0f;
  second.maxLod = -0.0f;
  const auto first_key = NativeSamplerCacheKey::FromCreateInfo(first);
  const auto second_key = NativeSamplerCacheKey::FromCreateInfo(second);
  CHECK(first_key == second_key);
  CHECK(NativeSamplerCacheKeyHash{}(first_key) == NativeSamplerCacheKeyHash{}(second_key));

  std::unordered_map<NativeSamplerCacheKey, uint32_t, NativeSamplerCacheKeyHash> cache;
  cache.emplace(first_key, 7);
  CHECK(cache.at(second_key) == 7);
}

TEST_CASE("Native sampler cache distinguishes active Vulkan sampler state") {
  VkSamplerCreateInfo baseline{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  baseline.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  baseline.anisotropyEnable = VK_TRUE;
  baseline.maxAnisotropy = 2.0f;
  baseline.maxLod = 4.0f;
  const auto key = NativeSamplerCacheKey::FromCreateInfo(baseline);

  auto changed = baseline;
  changed.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  CHECK_FALSE(key == NativeSamplerCacheKey::FromCreateInfo(changed));
  changed = baseline;
  changed.maxAnisotropy = 4.0f;
  CHECK_FALSE(key == NativeSamplerCacheKey::FromCreateInfo(changed));
  changed = baseline;
  changed.magFilter = VK_FILTER_LINEAR;
  CHECK_FALSE(key == NativeSamplerCacheKey::FromCreateInfo(changed));
  changed = baseline;
  changed.maxLod = 3.0f;
  CHECK_FALSE(key == NativeSamplerCacheKey::FromCreateInfo(changed));
  changed = baseline;
  changed.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
  CHECK_FALSE(key == NativeSamplerCacheKey::FromCreateInfo(changed));
}

TEST_CASE("Native sampler cache reserves auxiliary samplers without unsigned underflow") {
  CHECK_FALSE(CanAllocateNativeCachedSampler(0, 0));
  CHECK_FALSE(CanAllocateNativeCachedSampler(0, kNativeAuxiliarySamplerCount));
  CHECK(CanAllocateNativeCachedSampler(0, 10));
  CHECK_FALSE(CanAllocateNativeCachedSampler(1, 10));
  CHECK_FALSE(CanAllocateNativeCachedSampler(SIZE_MAX, UINT32_MAX));
}

}  // namespace
}  // namespace rex::graphics::gta4_native
