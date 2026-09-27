#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_BUFFER_ARENA_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_BUFFER_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rex::graphics::gta4_native {

enum class NativeBufferArenaStatus : uint8_t {
  kSuccess,
  kInvalidConfiguration,
  kInvalidSize,
  kOverflow,
  kCounterExhausted,
  kInvalidAllocation,
  kAllocationNotPending,
  kAllocationNotLive,
};

struct NativeBufferArenaConfig {
  uint64_t block_capacity = 0;
  uint64_t alignment = 0;
};

struct NativeBufferArenaAllocation {
  uint64_t id = 0;
  uint64_t block_id = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint64_t reserved_size = 0;
  uint64_t block_capacity = 0;
  bool new_block = false;
  bool dedicated = false;

  explicit operator bool() const { return id != 0 && block_id != 0; }
  bool operator==(const NativeBufferArenaAllocation&) const = default;
};

struct NativeBufferArenaResult {
  NativeBufferArenaStatus status = NativeBufferArenaStatus::kInvalidConfiguration;
  NativeBufferArenaAllocation allocation{};

  explicit operator bool() const { return status == NativeBufferArenaStatus::kSuccess; }
};

struct NativeBufferArenaRelease {
  NativeBufferArenaStatus status = NativeBufferArenaStatus::kInvalidConfiguration;
  uint64_t block_id = 0;
  bool destroy_block = false;

  explicit operator bool() const { return status == NativeBufferArenaStatus::kSuccess; }
};

struct NativeBufferArenaSnapshot {
  uint64_t block_count = 0;
  uint64_t live_allocation_count = 0;
  uint64_t pending_allocation_count = 0;
  uint64_t resident_capacity = 0;
  uint64_t live_reserved_bytes = 0;
  uint64_t free_bytes = 0;
  uint64_t allocation_hits = 0;
  uint64_t allocation_misses = 0;
};

// Deterministic best-fit suballocator for immutable native vertex/index data.
// Vulkan backing creation is transactional: Reserve carves a range, Commit
// publishes it after the buffer exists, and Cancel restores the exact range if
// creation or upload recording fails.
class NativeBufferArena final {
 public:
  explicit NativeBufferArena(NativeBufferArenaConfig config);

  bool valid() const { return valid_; }
  NativeBufferArenaResult Reserve(uint64_t size);
  NativeBufferArenaStatus Commit(uint64_t allocation_id);
  NativeBufferArenaRelease Cancel(uint64_t allocation_id);
  NativeBufferArenaRelease Release(uint64_t allocation_id);
  // Only entirely free, committed blocks can be removed. The caller releases
  // live allocations after CPU-owner expiry and last GPU submission completion.
  std::vector<uint64_t> TrimFreeBlocks(uint64_t retained_free_capacity,
                                       uint64_t completed_epoch = 0, uint64_t grace_epochs = 0);
  std::optional<NativeBufferArenaAllocation> GetAllocation(uint64_t allocation_id) const;
  NativeBufferArenaSnapshot Snapshot() const;

 private:
  enum class AllocationState : uint8_t {
    kPending,
    kLive,
  };

  struct Range {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  struct Block {
    uint64_t id = 0;
    uint64_t capacity = 0;
    bool dedicated = false;
    bool backing_ready = false;
    std::optional<uint64_t> empty_since_epoch;
    std::vector<Range> free_ranges;
  };

  struct AllocationRecord {
    NativeBufferArenaAllocation allocation{};
    AllocationState state = AllocationState::kPending;
  };

  bool AlignSize(uint64_t size, uint64_t& aligned_size) const;
  void InsertAndCoalesce(Block& block, Range range);
  NativeBufferArenaRelease ReleaseRecord(
      std::map<uint64_t, AllocationRecord>::iterator allocation, bool require_pending);

  NativeBufferArenaConfig config_{};
  bool valid_ = false;
  uint64_t next_block_id_ = 0;
  uint64_t next_allocation_id_ = 0;
  uint64_t allocation_hits_ = 0;
  uint64_t allocation_misses_ = 0;
  std::map<uint64_t, Block> blocks_;
  std::map<uint64_t, AllocationRecord> allocations_;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_BUFFER_ARENA_H_
