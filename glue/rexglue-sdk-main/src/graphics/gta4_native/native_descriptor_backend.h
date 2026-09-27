#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_DESCRIPTOR_BACKEND_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_DESCRIPTOR_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <vector>

namespace rex::graphics::gta4_native {

// MoltenVK exposes Metal argument-buffer-sized limits through Vulkan's
// UPDATE_AFTER_BIND limit family. The native renderer probes the exact fixed
// layout, requires UPDATE_UNUSED_WHILE_PENDING, then publishes stable bindless
// slots and defers reuse until the last referencing GPU submission completes.
enum class NativeDescriptorBackendRequest : uint8_t {
  kAuto,
  kIndexed,
  kCached,
};

enum class NativeDescriptorBackend : uint8_t {
  kIndexed,
  kCached,
};

// Opaque non-zero values supplied by the Vulkan integration. They commonly
// represent VkImageView and VkSampler handles, but keeping Vulkan out of this
// module makes the policy, page packing, and lifetime rules independently
// testable.
enum class NativeDescriptorKind : uint8_t {
  kSampledImage,
  kSampler,
};

enum class NativeDescriptorStatus : uint8_t {
  kSuccess,
  kInvalidConfiguration,
  kMissingIndexingFeatures,
  kMissingUpdateAfterBindFeatures,
  kInsufficientDeviceLimits,
  kLayoutSupportNotQueried,
  kLayoutUnsupported,
  kInvalidDescriptor,
  kCapacityExhausted,
  kInvalidSlot,
  kStaleSlot,
  kSlotNotLive,
  kInvalidFrameCopy,
  kFrameCopyInUse,
  kPendingWriteBatch,
  kInvalidWriteBatch,
  kNonMonotonicGpuSerial,
  kCounterExhausted,
  kGpuSubmissionPending,
  kDescriptorClearFailed,
};

struct NativeDescriptorIndexingFeatures {
  bool runtime_descriptor_array = false;
  bool descriptor_binding_partially_bound = false;
  bool descriptor_binding_sampled_image_update_after_bind = false;
  bool descriptor_binding_sampler_update_after_bind = false;
  bool descriptor_binding_update_unused_while_pending = false;
};

struct NativeDescriptorLimits {
  uint32_t max_per_stage_sampled_images = 0;
  uint32_t max_descriptor_set_sampled_images = 0;
  uint32_t max_per_stage_samplers = 0;
  uint32_t max_descriptor_set_samplers = 0;
  uint32_t max_per_stage_resources = 0;
  uint32_t max_descriptors_in_all_pools = 0;
};

struct NativeDescriptorCapacityInputs {
  NativeDescriptorLimits limits{};
  uint32_t desired_sampled_images_per_set = 0;
  uint32_t desired_samplers = 0;
  uint32_t minimum_sampled_images_per_set = 0;
  uint32_t minimum_samplers = 0;
  uint32_t sampled_image_set_count = 0;
  uint32_t frame_copy_count = 0;
  uint32_t storage_descriptors_per_copy = 0;
};

struct NativeDescriptorCapacity {
  NativeDescriptorStatus status = NativeDescriptorStatus::kInvalidConfiguration;
  uint32_t sampled_images_per_set = 0;
  uint32_t samplers = 0;
  uint32_t sampled_images_per_stage = 0;
  uint32_t resources_per_stage = 0;
  uint32_t pool_sampled_images = 0;
  uint32_t pool_samplers = 0;
  uint32_t pool_storage_buffers = 0;
  uint32_t pool_descriptors = 0;

  explicit operator bool() const { return status == NativeDescriptorStatus::kSuccess; }
};

// Chooses a compact bindless-table shape that is valid across every limit
// family used by the final pipeline layout and descriptor pool. Sampled-image
// capacity is prioritized because each texture occupies the same logical slot
// in four dimension-specific sets; sampler capacity receives the remaining
// combined-resource budget without dropping below its correctness floor.
NativeDescriptorCapacity ChooseNativeDescriptorCapacity(
    const NativeDescriptorCapacityInputs& inputs);

// A descriptor page binds four dimension-specific sampled-image sets and one
// sampler set. The layouts are fixed-size even though the shaders expose
// runtime arrays. The probe must call vkGetDescriptorSetLayoutSupport for the
// exact UPDATE_AFTER_BIND | PARTIALLY_BOUND | UPDATE_UNUSED_WHILE_PENDING
// layout proposed by the search.
struct NativeDescriptorPageCapacityInputs {
  NativeDescriptorLimits limits{};
  uint32_t desired_sampled_images_per_set = 0;
  uint32_t desired_samplers = 0;
  uint32_t minimum_sampled_images_per_set = 0;
  uint32_t minimum_samplers = 0;
  uint32_t sampled_image_set_count = 0;
  uint32_t frame_copy_count = 0;
  uint32_t storage_descriptors_per_copy = 0;
  uint32_t pool_storage_descriptors = 0;
};

using NativeDescriptorFixedLayoutProbe =
    std::function<bool(NativeDescriptorKind kind, uint32_t descriptor_count)>;

struct NativeDescriptorPageCapacity {
  NativeDescriptorStatus status = NativeDescriptorStatus::kInvalidConfiguration;
  uint32_t sampled_images_per_set = 0;
  uint32_t samplers = 0;
  uint32_t sampled_images_per_stage = 0;
  uint32_t resources_per_stage = 0;
  uint32_t descriptors_per_copy = 0;
  uint32_t descriptors_per_page = 0;
  uint32_t required_pool_descriptors = 0;
  uint32_t maximum_page_count = 0;
  uint32_t sampled_image_probe_count = 0;
  uint32_t sampler_probe_count = 0;

  explicit operator bool() const { return status == NativeDescriptorStatus::kSuccess; }
};

NativeDescriptorPageCapacity NegotiateNativeDescriptorPageCapacity(
    const NativeDescriptorPageCapacityInputs& inputs,
    const NativeDescriptorFixedLayoutProbe& probe);

// These values are produced by probing the exact proposed descriptor layouts
// with vkGetDescriptorSetLayoutSupport (and its variable-count pNext when
// applicable), rather than inferred from feature bits alone.
struct NativeDescriptorLayoutSupport {
  bool queried = false;
  bool supported = false;
  uint32_t supported_sampled_images = 0;
  uint32_t supported_samplers = 0;
};

struct NativeDescriptorPolicyInputs {
  NativeDescriptorBackendRequest request = NativeDescriptorBackendRequest::kAuto;
  NativeDescriptorIndexingFeatures indexing_features{};
  NativeDescriptorLimits limits{};
  NativeDescriptorLayoutSupport layout_support{};
  uint32_t required_sampled_images = 0;
  uint32_t required_samplers = 0;
  uint32_t required_per_stage_resources = 0;
  uint32_t required_pool_descriptors = 0;
  // MoltenVK exposes Metal argument-buffer-sized limits through the
  // update-after-bind limit family. The renderer publishes stable slots in a
  // single global table and reclaims a slot only after its last committed GPU
  // submission completes. UPDATE_UNUSED_WHILE_PENDING makes that isolated
  // descriptor update legal while later submissions use other table slots.
  bool use_update_after_bind_layout = false;
};

struct NativeDescriptorPolicyDecision {
  NativeDescriptorStatus status = NativeDescriptorStatus::kInvalidConfiguration;
  NativeDescriptorBackend backend = NativeDescriptorBackend::kCached;
  // kSuccess means indexed mode was accepted. In auto mode, any other value
  // explains why the safe cached backend was selected while status remains
  // kSuccess.
  NativeDescriptorStatus indexed_rejection = NativeDescriptorStatus::kSuccess;

  explicit operator bool() const { return status == NativeDescriptorStatus::kSuccess; }
};

NativeDescriptorPolicyDecision ChooseNativeDescriptorBackend(
    const NativeDescriptorPolicyInputs& inputs);

struct NativeDescriptorPayload {
  uint64_t image_view = 0;
  uint64_t sampler = 0;

  bool valid(NativeDescriptorKind kind) const {
    return kind == NativeDescriptorKind::kSampledImage ? image_view != 0 : sampler != 0;
  }
  bool operator==(const NativeDescriptorPayload&) const = default;
};

struct NativeDescriptorSlotHandle {
  uint32_t index = std::numeric_limits<uint32_t>::max();
  uint64_t generation = 0;
  uint32_t page = 0;

  bool operator==(const NativeDescriptorSlotHandle&) const = default;
};

struct NativeDescriptorAllocation {
  NativeDescriptorStatus status = NativeDescriptorStatus::kInvalidConfiguration;
  NativeDescriptorSlotHandle handle{};

  explicit operator bool() const { return status == NativeDescriptorStatus::kSuccess; }
};

struct NativeDescriptorWrite {
  uint32_t slot = 0;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  NativeDescriptorPayload payload{};
  bool tombstone = false;
};

struct NativeDescriptorWriteBatch {
  NativeDescriptorStatus status = NativeDescriptorStatus::kInvalidConfiguration;
  uint32_t frame_copy = 0;
  uint64_t batch_id = 0;
  std::vector<NativeDescriptorWrite> writes;

  explicit operator bool() const { return status == NativeDescriptorStatus::kSuccess; }
};

struct NativeDescriptorEpochConfig {
  uint32_t frame_copy_count = 0;
  uint32_t slot_capacity = 0;
  NativeDescriptorKind kind = NativeDescriptorKind::kSampledImage;
  NativeDescriptorPayload fallback{};
};

class NativeDescriptorEpochTable {
 public:
  explicit NativeDescriptorEpochTable(const NativeDescriptorEpochConfig& config);

  NativeDescriptorEpochTable(const NativeDescriptorEpochTable&) = delete;
  NativeDescriptorEpochTable& operator=(const NativeDescriptorEpochTable&) = delete;
  NativeDescriptorEpochTable(NativeDescriptorEpochTable&&) = default;
  NativeDescriptorEpochTable& operator=(NativeDescriptorEpochTable&&) = default;

  bool valid() const { return initialization_status_ == NativeDescriptorStatus::kSuccess; }
  NativeDescriptorStatus initialization_status() const { return initialization_status_; }

  NativeDescriptorAllocation Allocate(const NativeDescriptorPayload& payload);
  NativeDescriptorStatus Update(NativeDescriptorSlotHandle handle,
                                const NativeDescriptorPayload& payload);
  NativeDescriptorStatus Retire(NativeDescriptorSlotHandle handle, uint64_t last_use_gpu_serial);

  // A frame copy may only be updated after its last submitted GPU serial has
  // completed. Begin returns concrete, always-valid writes. Commit must be
  // called only after those writes have been successfully applied to Vulkan.
  NativeDescriptorWriteBatch BeginFrameCopyWrites(uint32_t frame_copy);
  NativeDescriptorStatus CommitFrameCopyWrites(const NativeDescriptorWriteBatch& batch);
  NativeDescriptorStatus CancelFrameCopyWrites(const NativeDescriptorWriteBatch& batch);

  NativeDescriptorStatus MarkFrameCopySubmitted(uint32_t frame_copy, uint64_t gpu_serial);
  NativeDescriptorStatus MarkGpuCompleted(uint64_t gpu_serial);
  size_t ReclaimReadySlots();

  bool IsLive(NativeDescriptorSlotHandle handle) const;
  // True once the retired generation's tombstone has reached every frame
  // copy and its last GPU use has completed. A stale generation is also
  // complete because recycling is only possible after those conditions.
  bool IsRetirementComplete(NativeDescriptorSlotHandle handle) const;
  size_t live_count() const { return live_count_; }
  size_t retiring_count() const { return retiring_count_; }
  size_t free_count() const { return free_slots_.size(); }
  size_t pending_write_slot_count(uint32_t frame_copy) const {
    return frame_copy < dirty_slots_by_copy_.size() ? dirty_slots_by_copy_[frame_copy].size() : 0;
  }
  size_t retirement_candidate_count() const { return retiring_slots_.size(); }
  uint64_t completed_gpu_serial() const { return completed_gpu_serial_; }
  NativeDescriptorKind kind() const { return kind_; }
  const NativeDescriptorPayload& fallback() const { return fallback_; }

 private:
  enum class SlotState : uint8_t {
    kFree,
    kLive,
    kRetiring,
  };

  struct Slot {
    SlotState state = SlotState::kFree;
    uint64_t generation = 0;
    uint64_t epoch = 0;
    uint64_t tombstone_epoch = 0;
    uint64_t retire_after_gpu_serial = 0;
    NativeDescriptorPayload payload{};
  };

  struct PendingBatch {
    uint64_t id = 0;
    std::vector<NativeDescriptorWrite> writes;
  };

  bool HasPendingBatch() const;
  NativeDescriptorStatus ValidateLiveHandle(NativeDescriptorSlotHandle handle) const;
  bool AdvanceEpoch(uint64_t* next_epoch);
  size_t AppliedEpochOffset(uint32_t frame_copy, uint32_t slot) const;
  void MarkSlotDirty(uint32_t slot);

  NativeDescriptorStatus initialization_status_ = NativeDescriptorStatus::kInvalidConfiguration;
  NativeDescriptorKind kind_ = NativeDescriptorKind::kSampledImage;
  NativeDescriptorPayload fallback_{};
  std::vector<Slot> slots_;
  std::deque<uint32_t> free_slots_;
  std::vector<uint64_t> applied_epochs_;
  std::vector<std::vector<uint32_t>> dirty_slots_by_copy_;
  std::vector<uint8_t> dirty_slot_membership_;
  std::vector<uint32_t> retiring_slots_;
  std::vector<uint64_t> frame_copy_submission_serials_;
  std::vector<PendingBatch> pending_batches_;
  uint64_t current_epoch_ = 0;
  uint64_t next_batch_id_ = 0;
  uint64_t completed_gpu_serial_ = 0;
  uint64_t greatest_submitted_gpu_serial_ = 0;
  size_t live_count_ = 0;
  size_t retiring_count_ = 0;
};

struct NativeStableDescriptorSlotConfig {
  uint32_t page = 0;
  uint32_t slot_capacity = 0;
};

struct NativeStableDescriptorSlotCounters {
  uint64_t allocation_calls = 0;
  uint64_t abort_calls = 0;
  uint64_t retirement_calls = 0;
  uint64_t reclaim_calls = 0;
  uint64_t publish_requirements = 0;
  uint64_t aborted_unsubmitted_slots = 0;
  uint64_t clear_callbacks = 0;
  uint64_t reclaimed_slots = 0;
};

// Owns stable bindless locations for one fixed descriptor page. Allocation
// publishes one descriptor exactly once. Retirement only changes CPU state;
// the caller journals the handle until its last GPU submission completes.
// Reclaim invokes the supplied clear callback exactly once before making the
// slot available again, so a generation can never be recycled early.
class NativeStableDescriptorSlotTable {
 public:
  using ClearCallback = std::function<bool(NativeDescriptorSlotHandle)>;

  explicit NativeStableDescriptorSlotTable(const NativeStableDescriptorSlotConfig& config);

  NativeStableDescriptorSlotTable(const NativeStableDescriptorSlotTable&) = delete;
  NativeStableDescriptorSlotTable& operator=(const NativeStableDescriptorSlotTable&) = delete;
  NativeStableDescriptorSlotTable(NativeStableDescriptorSlotTable&&) = default;
  NativeStableDescriptorSlotTable& operator=(NativeStableDescriptorSlotTable&&) = default;

  bool valid() const { return initialization_status_ == NativeDescriptorStatus::kSuccess; }
  NativeDescriptorStatus initialization_status() const { return initialization_status_; }

  NativeDescriptorAllocation Allocate();
  // Returns a slot to the O(1) freelist when descriptor publication failed
  // before any submitted command could reference it. Submitted slots must use
  // Retire/Reclaim instead.
  NativeDescriptorStatus AbortUnsubmitted(NativeDescriptorSlotHandle handle);
  NativeDescriptorStatus Retire(NativeDescriptorSlotHandle handle,
                                uint64_t last_use_gpu_serial);
  NativeDescriptorStatus Reclaim(NativeDescriptorSlotHandle handle,
                                 uint64_t completed_gpu_serial,
                                 const ClearCallback& clear_callback);

  bool IsLive(NativeDescriptorSlotHandle handle) const;
  bool IsRetiring(NativeDescriptorSlotHandle handle) const;
  bool IsRetirementComplete(NativeDescriptorSlotHandle handle) const;
  uint32_t page() const { return page_; }
  uint32_t capacity() const { return uint32_t(slots_.size()); }
  size_t live_count() const { return live_count_; }
  size_t retiring_count() const { return retiring_count_; }
  size_t free_count() const { return free_slots_.size(); }
  uint64_t semantic_epoch() const { return semantic_epoch_; }
  const NativeStableDescriptorSlotCounters& counters() const { return counters_; }

 private:
  enum class SlotState : uint8_t {
    kFree,
    kLive,
    kRetiring,
  };

  struct Slot {
    SlotState state = SlotState::kFree;
    uint64_t generation = 0;
    uint64_t retire_after_gpu_serial = 0;
  };

  NativeDescriptorStatus ValidateHandle(NativeDescriptorSlotHandle handle) const;
  bool AdvanceSemanticEpoch();

  NativeDescriptorStatus initialization_status_ = NativeDescriptorStatus::kInvalidConfiguration;
  uint32_t page_ = 0;
  std::vector<Slot> slots_;
  std::deque<uint32_t> free_slots_;
  uint64_t semantic_epoch_ = 0;
  size_t live_count_ = 0;
  size_t retiring_count_ = 0;
  NativeStableDescriptorSlotCounters counters_{};
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_DESCRIPTOR_BACKEND_H_
