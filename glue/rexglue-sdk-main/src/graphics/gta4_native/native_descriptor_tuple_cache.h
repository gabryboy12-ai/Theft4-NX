#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::gta4_native {

struct NativeDescriptorNoDependencies {
  template <typename Key>
  constexpr std::array<uint64_t, 0> operator()(const Key&) const {
    return {};
  }
};

// Ordinary descriptor bundles are immutable from publication until BOTH their
// submission completes and the slot's old command buffers have been discarded.
// Retirement removes future lookup immediately; it never rewrites a pending set.
// The caller supplies lifetime-aware keys and owns the bounded Vulkan pools.
template <typename Key, typename Bundle, typename Hash,
          typename Dependencies = NativeDescriptorNoDependencies>
class NativeDescriptorTupleCache {
 public:
  explicit NativeDescriptorTupleCache(size_t capacity) : capacity_(capacity) {}
  // The reverse index refers to unordered_map keys, whose addresses survive
  // rehash and container moves. Copying would leave pointers into another cache.
  NativeDescriptorTupleCache(const NativeDescriptorTupleCache&) = delete;
  NativeDescriptorTupleCache& operator=(const NativeDescriptorTupleCache&) = delete;
  NativeDescriptorTupleCache(NativeDescriptorTupleCache&&) = default;
  NativeDescriptorTupleCache& operator=(NativeDescriptorTupleCache&&) = default;

  bool BeginAfterCommandPoolReset(uint64_t completed_submission) {
    if (last_submission_ > completed_submission) {
      return false;
    }
    reusable_.insert(reusable_.end(), retired_.begin(), retired_.end());
    retired_.clear();
    writable_ = true;
    return true;
  }

  void MarkSubmitted(uint64_t submission) {
    last_submission_ = std::max(last_submission_, submission);
    writable_ = false;
  }

  // Select the persistent subset before any draw can reference these sets.
  // Entries in the current frame are never evicted. Excess distinct tuples
  // must use the caller's submission-owned transient pool.
  void Prepare(const std::unordered_set<Key, Hash>& required) {
    if (!writable_) {
      return;
    }
    const size_t desired = std::min(required.size(), capacity_);
    size_t present = 0;
    for (const auto& key : required) {
      present += entries_.contains(key);
    }
    size_t missing = desired > present ? desired - present : 0;
    for (auto it = entries_.begin(); it != entries_.end() &&
                                         capacity_ - entries_.size() < missing;) {
      if (required.contains(it->first)) {
        ++it;
      } else {
        reusable_.push_back(it->second);
        RemoveDependencies(it->first);
        it = entries_.erase(it);
      }
    }
  }

  template <typename Predicate>
  void Invalidate(Predicate references_retired_resource) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!references_retired_resource(it->first)) {
        ++it;
      } else {
        retired_.push_back(it->second);
        RemoveDependencies(it->first);
        it = entries_.erase(it);
      }
    }
  }

  // Visit only tuples that reference this lifetime. Repeated shader slots are
  // deduplicated by the set, and zero denotes an absent resource. Detach the
  // bucket before erasing keys so cross-dependency cleanup cannot invalidate it.
  void InvalidateDependency(uint64_t lifetime) {
    auto dependency = dependency_keys_.extract(lifetime);
    if (dependency.empty()) {
      return;
    }
    for (const Key* key : dependency.mapped()) {
      const auto entry = entries_.find(*key);
      retired_.push_back(entry->second);
      RemoveDependencies(entry->first);
      entries_.erase(entry);
    }
  }

  const Bundle* Find(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
  }
  bool Insert(const Key& key, const Bundle& bundle) {
    if (!writable_ || entries_.size() >= capacity_) {
      return false;
    }
    const auto [entry, inserted] = entries_.emplace(key, bundle);
    if (inserted) {
      for (const uint64_t lifetime : Dependencies{}(entry->first)) {
        if (lifetime) {
          dependency_keys_[lifetime].insert(&entry->first);
        }
      }
    }
    return inserted;
  }
  std::optional<Bundle> TakeReusable() {
    if (!writable_ || reusable_.empty()) {
      return std::nullopt;
    }
    Bundle bundle = reusable_.back();
    reusable_.pop_back();
    return bundle;
  }
  size_t size() const { return entries_.size(); }
  size_t capacity() const { return capacity_; }
  size_t reusable_count() const { return reusable_.size(); }
  size_t retired_count() const { return retired_.size(); }
  bool writable() const { return writable_; }

 private:
  void RemoveDependencies(const Key& key) {
    for (const uint64_t lifetime : Dependencies{}(key)) {
      if (!lifetime) {
        continue;
      }
      const auto dependency = dependency_keys_.find(lifetime);
      if (dependency == dependency_keys_.end()) {
        continue;
      }
      dependency->second.erase(&key);
      if (dependency->second.empty()) {
        dependency_keys_.erase(dependency);
      }
    }
  }

  size_t capacity_;
  uint64_t last_submission_ = 0;
  bool writable_ = false;
  std::unordered_map<Key, Bundle, Hash> entries_;
  std::unordered_map<uint64_t, std::unordered_set<const Key*>> dependency_keys_;
  std::vector<Bundle> reusable_;
  std::vector<Bundle> retired_;
};

}  // namespace rex::graphics::gta4_native
