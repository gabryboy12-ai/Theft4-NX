#pragma once
/**
 * Copyright 2026. Released under the BSD license - see LICENSE.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

namespace rex::graphics::vulkan {

// Dynamic offsets are bind state, not immutable descriptor-set contents.
// Retain every buffer and its exact accessible range in the descriptor key.
template <size_t N>
struct DynamicUniformDescriptorKey {
  std::array<VkBuffer, N> buffers{};
  std::array<VkDeviceSize, N> ranges{};
  bool operator==(const DynamicUniformDescriptorKey& other) const {
    return buffers == other.buffers && ranges == other.ranges;
  }
  struct Hasher {
    size_t operator()(const DynamicUniformDescriptorKey& key) const {
      size_t hash = 0;
      const auto append = [&hash](size_t value) {
        hash ^= value + size_t(0x9E3779B9) + (hash << 6) + (hash >> 2);
      };
      for (size_t i = 0; i < N; ++i) {
        append(std::hash<VkBuffer>{}(key.buffers[i]));
        append(std::hash<VkDeviceSize>{}(key.ranges[i]));
      }
      return hash;
    }
  };
};

// Preserve exactly the original GPU address and visible byte range while
// moving the allocation offset from each descriptor to vkCmdBindDescriptorSets.
// Reject unusual ranges/offsets so callers can retain their original path.
template <size_t N>
bool NormalizeDynamicUniformDescriptors(
    const VkDescriptorBufferInfo* original, VkDeviceSize alignment,
    VkDeviceSize page_size, VkDeviceSize range_limit,
    std::array<VkDescriptorBufferInfo, N>& normalized,
    std::array<uint32_t, N>& dynamic_offsets, DynamicUniformDescriptorKey<N>& key) {
  if (!alignment || !page_size || !range_limit) return false;
  for (size_t i = 0; i < N; ++i) {
    const auto& info = original[i];
    if (info.buffer == VK_NULL_HANDLE || !info.range || info.range == VK_WHOLE_SIZE ||
        info.range > range_limit || info.offset > UINT32_MAX ||
        info.offset % alignment || info.offset > page_size ||
        info.range > page_size - info.offset) {
      return false;
    }
  }
  // Publish the output only after validating all bindings, including unused
  // pixel/vertex bindings required by the common pipeline layout.
  for (size_t i = 0; i < N; ++i) {
    normalized[i] = {original[i].buffer, 0, original[i].range};
    dynamic_offsets[i] = uint32_t(original[i].offset);
    key.buffers[i] = original[i].buffer;
    key.ranges[i] = original[i].range;
  }
  return true;
}

template <size_t N, size_t MaxEntries = 1024>
class ImmutableDynamicUniformDescriptorCache {
 public:
  using Key = DynamicUniformDescriptorKey<N>;
  static constexpr size_t kMaxEntries = MaxEntries;
  void BeginFrame(uint64_t frame) {
    if (!epoch_valid_ || epoch_ != frame) {
      sets_.clear();
      epoch_ = frame;
      epoch_valid_ = true;
    }
  }
  // Clear before pool destruction or buffer-handle recycling. Sets remain
  // owned and fenced by the caller's existing transient descriptor allocator.
  void Clear() { sets_.clear(); epoch_valid_ = false; }
  VkDescriptorSet Find(const Key& key) const {
    if (!epoch_valid_) return VK_NULL_HANDLE;
    const auto it = sets_.find(key);
    return it == sets_.end() ? VK_NULL_HANDLE : it->second;
  }
  bool Publish(const Key& key, VkDescriptorSet set) {
    if (!epoch_valid_ || set == VK_NULL_HANDLE) return false;
    const auto found = sets_.find(key);
    if (found != sets_.end()) return found->second == set;
    // A full cache affects performance only: callers still allocate and write
    // a normal immutable set, without retaining another cache entry.
    if (sets_.size() >= MaxEntries) return false;
    sets_.emplace(key, set);
    return true;
  }
  size_t size() const { return sets_.size(); }
 private:
  std::unordered_map<Key, VkDescriptorSet, typename Key::Hasher> sets_;
  uint64_t epoch_ = 0;
  bool epoch_valid_ = false;
};

}  // namespace rex::graphics::vulkan
