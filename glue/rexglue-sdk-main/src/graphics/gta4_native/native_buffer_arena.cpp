#include "native_buffer_arena.h"

#include <algorithm>
#include <limits>

namespace rex::graphics::gta4_native {

NativeBufferArena::NativeBufferArena(NativeBufferArenaConfig config) : config_(config) {
  valid_ = config_.block_capacity != 0 && config_.alignment != 0 &&
           (config_.alignment & (config_.alignment - 1)) == 0 &&
           (config_.block_capacity & (config_.alignment - 1)) == 0;
}

bool NativeBufferArena::AlignSize(uint64_t size, uint64_t& aligned_size) const {
  if (!valid_ || !size) {
    return false;
  }
  const uint64_t mask = config_.alignment - 1;
  if (size > std::numeric_limits<uint64_t>::max() - mask) {
    return false;
  }
  aligned_size = (size + mask) & ~mask;
  return aligned_size != 0;
}

NativeBufferArenaResult NativeBufferArena::Reserve(uint64_t size) {
  NativeBufferArenaResult result{};
  if (!valid_) {
    result.status = NativeBufferArenaStatus::kInvalidConfiguration;
    return result;
  }
  uint64_t reserved_size = 0;
  if (!size) {
    result.status = NativeBufferArenaStatus::kInvalidSize;
    return result;
  }
  if (!AlignSize(size, reserved_size)) {
    result.status = NativeBufferArenaStatus::kOverflow;
    return result;
  }
  if (next_allocation_id_ == std::numeric_limits<uint64_t>::max() ||
      next_block_id_ == std::numeric_limits<uint64_t>::max()) {
    result.status = NativeBufferArenaStatus::kCounterExhausted;
    return result;
  }

  Block* best_block = nullptr;
  size_t best_range_index = 0;
  uint64_t best_waste = std::numeric_limits<uint64_t>::max();
  if (reserved_size <= config_.block_capacity) {
    for (auto& [block_id, block] : blocks_) {
      (void)block_id;
      if (block.dedicated || !block.backing_ready) {
        continue;
      }
      for (size_t range_index = 0; range_index < block.free_ranges.size(); ++range_index) {
        const Range& range = block.free_ranges[range_index];
        if (range.size < reserved_size) {
          continue;
        }
        const uint64_t waste = range.size - reserved_size;
        if (!best_block || waste < best_waste ||
            (waste == best_waste && block.id < best_block->id) ||
            (waste == best_waste && block.id == best_block->id &&
             range.offset < best_block->free_ranges[best_range_index].offset)) {
          best_block = &block;
          best_range_index = range_index;
          best_waste = waste;
        }
      }
    }
  }

  bool new_block = best_block == nullptr;
  if (new_block) {
    ++next_block_id_;
    Block block{};
    block.id = next_block_id_;
    block.capacity = std::max(config_.block_capacity, reserved_size);
    block.dedicated = reserved_size > config_.block_capacity;
    auto [entry, inserted] = blocks_.emplace(block.id, std::move(block));
    if (!inserted) {
      result.status = NativeBufferArenaStatus::kCounterExhausted;
      return result;
    }
    best_block = &entry->second;
    ++allocation_misses_;
  } else {
    ++allocation_hits_;
  }

  uint64_t offset = 0;
  best_block->empty_since_epoch.reset();
  if (!new_block) {
    Range& range = best_block->free_ranges[best_range_index];
    offset = range.offset;
    range.offset += reserved_size;
    range.size -= reserved_size;
    if (!range.size) {
      best_block->free_ranges.erase(best_block->free_ranges.begin() + best_range_index);
    }
  }

  ++next_allocation_id_;
  NativeBufferArenaAllocation allocation{};
  allocation.id = next_allocation_id_;
  allocation.block_id = best_block->id;
  allocation.offset = offset;
  allocation.size = size;
  allocation.reserved_size = reserved_size;
  allocation.block_capacity = best_block->capacity;
  allocation.new_block = new_block;
  allocation.dedicated = best_block->dedicated;
  allocations_.emplace(allocation.id,
                       AllocationRecord{allocation, AllocationState::kPending});
  result.status = NativeBufferArenaStatus::kSuccess;
  result.allocation = allocation;
  return result;
}

NativeBufferArenaStatus NativeBufferArena::Commit(uint64_t allocation_id) {
  const auto allocation = allocations_.find(allocation_id);
  if (allocation == allocations_.end()) {
    return NativeBufferArenaStatus::kInvalidAllocation;
  }
  if (allocation->second.state != AllocationState::kPending) {
    return NativeBufferArenaStatus::kAllocationNotPending;
  }
  auto block = blocks_.find(allocation->second.allocation.block_id);
  if (block == blocks_.end()) {
    return NativeBufferArenaStatus::kInvalidAllocation;
  }
  if (allocation->second.allocation.new_block) {
    block->second.backing_ready = true;
    const uint64_t used_end = allocation->second.allocation.reserved_size;
    if (used_end < block->second.capacity) {
      block->second.free_ranges.push_back({used_end, block->second.capacity - used_end});
    }
  } else if (!block->second.backing_ready) {
    return NativeBufferArenaStatus::kInvalidAllocation;
  }
  allocation->second.state = AllocationState::kLive;
  return NativeBufferArenaStatus::kSuccess;
}

void NativeBufferArena::InsertAndCoalesce(Block& block, Range range) {
  const auto insertion = std::lower_bound(
      block.free_ranges.begin(), block.free_ranges.end(), range.offset,
      [](const Range& candidate, uint64_t offset) { return candidate.offset < offset; });
  size_t insertion_index = static_cast<size_t>(insertion - block.free_ranges.begin());
  block.free_ranges.insert(insertion, range);
  if (insertion_index != 0) {
    const size_t previous_index = insertion_index - 1;
    Range& previous = block.free_ranges[previous_index];
    const Range& inserted = block.free_ranges[insertion_index];
    if (previous.offset + previous.size == inserted.offset) {
      previous.size += inserted.size;
      block.free_ranges.erase(block.free_ranges.begin() + insertion_index);
      insertion_index = previous_index;
    }
  }
  const size_t next_index = insertion_index + 1;
  if (next_index < block.free_ranges.size()) {
    Range& inserted = block.free_ranges[insertion_index];
    const Range& next = block.free_ranges[next_index];
    if (inserted.offset + inserted.size == next.offset) {
      inserted.size += next.size;
      block.free_ranges.erase(block.free_ranges.begin() + next_index);
    }
  }
}

NativeBufferArenaRelease NativeBufferArena::ReleaseRecord(
    std::map<uint64_t, AllocationRecord>::iterator allocation, bool require_pending) {
  NativeBufferArenaRelease result{};
  if (allocation == allocations_.end()) {
    result.status = NativeBufferArenaStatus::kInvalidAllocation;
    return result;
  }
  if (require_pending && allocation->second.state != AllocationState::kPending) {
    result.status = NativeBufferArenaStatus::kAllocationNotPending;
    return result;
  }
  if (!require_pending && allocation->second.state != AllocationState::kLive) {
    result.status = NativeBufferArenaStatus::kAllocationNotLive;
    return result;
  }
  const NativeBufferArenaAllocation released = allocation->second.allocation;
  auto block = blocks_.find(released.block_id);
  if (block == blocks_.end()) {
    result.status = NativeBufferArenaStatus::kInvalidAllocation;
    return result;
  }

  result.status = NativeBufferArenaStatus::kSuccess;
  result.block_id = released.block_id;
  if (released.new_block && require_pending) {
    blocks_.erase(block);
    // The caller may already have created a Vulkan backing between Reserve and
    // Commit. Returning true is harmless when it has not and prevents a
    // partially-created backing from leaking on a failed upload transaction.
    result.destroy_block = true;
  } else if (block->second.dedicated) {
    blocks_.erase(block);
    result.destroy_block = true;
  } else {
    InsertAndCoalesce(block->second, {released.offset, released.reserved_size});
  }
  allocations_.erase(allocation);
  return result;
}

NativeBufferArenaRelease NativeBufferArena::Cancel(uint64_t allocation_id) {
  return ReleaseRecord(allocations_.find(allocation_id), true);
}

NativeBufferArenaRelease NativeBufferArena::Release(uint64_t allocation_id) {
  return ReleaseRecord(allocations_.find(allocation_id), false);
}

std::vector<uint64_t> NativeBufferArena::TrimFreeBlocks(uint64_t retained_free_capacity,
                                                       uint64_t completed_epoch,
                                                       uint64_t grace_epochs) {
  std::vector<uint64_t> removed;
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    Block& block = it->second;
    if (!block.backing_ready || block.free_ranges.size() != 1 ||
        block.free_ranges.front().offset != 0 ||
        block.free_ranges.front().size != block.capacity) {
      block.empty_since_epoch.reset();
      ++it;
      continue;
    }
    if (!block.empty_since_epoch) {
      block.empty_since_epoch = completed_epoch;
    }
    if (block.capacity <= retained_free_capacity) {
      retained_free_capacity -= block.capacity;
      ++it;
      continue;
    }
    if (completed_epoch < *block.empty_since_epoch ||
        completed_epoch - *block.empty_since_epoch < grace_epochs) {
      ++it;
      continue;
    }
    removed.push_back(block.id);
    it = blocks_.erase(it);
  }
  return removed;
}

std::optional<NativeBufferArenaAllocation> NativeBufferArena::GetAllocation(
    uint64_t allocation_id) const {
  const auto allocation = allocations_.find(allocation_id);
  return allocation == allocations_.end()
             ? std::nullopt
             : std::optional<NativeBufferArenaAllocation>(allocation->second.allocation);
}

NativeBufferArenaSnapshot NativeBufferArena::Snapshot() const {
  NativeBufferArenaSnapshot snapshot{};
  snapshot.block_count = blocks_.size();
  snapshot.allocation_hits = allocation_hits_;
  snapshot.allocation_misses = allocation_misses_;
  for (const auto& [block_id, block] : blocks_) {
    (void)block_id;
    snapshot.resident_capacity += block.capacity;
    for (const Range& range : block.free_ranges) {
      snapshot.free_bytes += range.size;
    }
  }
  for (const auto& [allocation_id, record] : allocations_) {
    (void)allocation_id;
    if (record.state == AllocationState::kLive) {
      ++snapshot.live_allocation_count;
      snapshot.live_reserved_bytes += record.allocation.reserved_size;
    } else {
      ++snapshot.pending_allocation_count;
    }
  }
  return snapshot;
}

}  // namespace rex::graphics::gta4_native
