#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_SUBMISSION_LIFETIME_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_SUBMISSION_LIFETIME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace rex::graphics::gta4_native {

// Thread-safe CPU-side ownership model for an ordered graphics queue. The
// producer may queue resource references while the render worker records or
// completes another frame. Vulkan objects remain owned by the integrating
// renderer; this class only decides when reuse and destruction are legal.
class NativeSubmissionLifetime final {
 public:
  using SubmissionSerial = uint64_t;
  using DescriptorEpoch = uint64_t;

  static constexpr size_t kFrameSlotCount = 2;

  struct ResourceKey {
    uint64_t identity = 0;
    uint64_t generation = 0;

    constexpr bool operator==(const ResourceKey&) const = default;
    constexpr bool operator<(const ResourceKey& other) const {
      return identity < other.identity ||
             (identity == other.identity && generation < other.generation);
    }
  };

  struct FrameToken {
    uint32_t slot = UINT32_MAX;
    uint64_t generation = 0;

    constexpr bool operator==(const FrameToken&) const = default;
  };

  enum class FrameSlotState : uint8_t {
    kAvailable,
    kRecording,
    kPrepared,
    kSubmitted,
  };

  struct FrameSlotSnapshot {
    FrameSlotState state = FrameSlotState::kAvailable;
    uint64_t generation = 0;
    SubmissionSerial submission_serial = 0;
    DescriptorEpoch applied_descriptor_epoch = 0;
    size_t staged_resource_count = 0;
    uint64_t staged_reference_count = 0;
  };

  struct ResourceSnapshot {
    bool descriptor_backed = false;
    bool retirement_requested = false;
    uint64_t queued_reference_count = 0;
    SubmissionSerial last_use_serial = 0;
    DescriptorEpoch tombstone_epoch = 0;
  };

  struct DescriptorTombstone {
    ResourceKey resource{};
    DescriptorEpoch epoch = 0;

    constexpr bool operator==(const DescriptorTombstone&) const = default;
  };

  // Resource generations are immutable. Re-registering the same key or using
  // the reserved all-zero key is rejected.
  bool RegisterResource(ResourceKey key, bool descriptor_backed);

  // Adds references held by frames or commands that have not been submitted.
  // References cannot be added after retirement has been requested.
  bool QueueReference(ResourceKey key, uint64_t count = 1);

  // A slot may begin recording only after its previous fence was reported
  // complete. Tokens contain a slot generation so stale callbacks cannot
  // mutate a later frame using the same slot.
  std::optional<FrameToken> BeginFrame(uint32_t slot);
  bool StageQueuedReference(FrameToken token, ResourceKey key, uint64_t count = 1);

  // Preparing consumes a unique monotonically increasing CPU serial, but does
  // not change any resource lifetime. Call CommitSubmission only after the
  // queue accepted the submission. RollbackFrame preserves all queued
  // references and intentionally does not reuse a failed submission serial.
  std::optional<SubmissionSerial> PrepareSubmission(FrameToken token);
  bool CommitSubmission(FrameToken token, SubmissionSerial serial);
  bool RollbackFrame(FrameToken token);

  // Drops a frame permanently. Unlike rollback, all references staged for the
  // dropped frame are released without being marked as GPU uses.
  bool AbandonFrame(FrameToken token);

  // Reports the fence associated with this exact slot submission. Submissions
  // are assumed to target one ordered queue, so completion of a later serial
  // also proves completion of every earlier successfully submitted serial.
  bool NotifySlotFenceCompleted(uint32_t slot, SubmissionSerial serial);

  // Retirement is idempotent. Descriptor-backed resources receive one global
  // tombstone epoch; non-descriptor resources return epoch zero.
  std::optional<DescriptorEpoch> RequestRetirement(ResourceKey key);

  // Returns the exact tombstones needed to advance one available descriptor
  // table copy. The query is transactional: abandoning the returned journal
  // changes nothing. CommitDescriptorTombstones acknowledges that the caller
  // applied the complete journal successfully.
  std::optional<std::vector<DescriptorTombstone>> GetDescriptorTombstones(
      uint32_t slot, DescriptorEpoch target_epoch) const;
  bool CommitDescriptorTombstones(uint32_t slot, DescriptorEpoch target_epoch);

  // Destruction requires all three independent conditions: no queued CPU
  // references, completion of the last GPU use, and propagation of the
  // descriptor tombstone to both frame-slot table copies.
  bool CanRetire(ResourceKey key) const;
  bool EraseRetiredResource(ResourceKey key);

  std::optional<FrameSlotSnapshot> GetFrameSlotSnapshot(uint32_t slot) const;
  std::optional<ResourceSnapshot> GetResourceSnapshot(ResourceKey key) const;
  SubmissionSerial completed_submission_serial() const;
  SubmissionSerial last_issued_submission_serial() const;
  DescriptorEpoch last_issued_descriptor_epoch() const;

 private:
  struct FrameSlot {
    FrameSlotState state = FrameSlotState::kAvailable;
    uint64_t generation = 0;
    SubmissionSerial submission_serial = 0;
    DescriptorEpoch applied_descriptor_epoch = 0;
    std::map<ResourceKey, uint64_t> staged_references;
  };

  struct ResourceState {
    bool descriptor_backed = false;
    bool retirement_requested = false;
    uint64_t queued_reference_count = 0;
    SubmissionSerial last_use_serial = 0;
    DescriptorEpoch tombstone_epoch = 0;
  };

  bool IsValidTokenLocked(FrameToken token) const;
  std::optional<uint64_t> GetStagedReferenceCountLocked(ResourceKey key) const;
  bool CanApplyDescriptorEpochLocked(uint32_t slot, DescriptorEpoch target_epoch) const;
  bool CanRetireLocked(const ResourceState& resource) const;
  void ResetSlotLocked(FrameSlot& slot);

  mutable std::mutex mutex_;
  std::array<FrameSlot, kFrameSlotCount> frame_slots_{};
  std::map<ResourceKey, ResourceState> resources_;
  std::map<DescriptorEpoch, ResourceKey> descriptor_tombstones_;
  SubmissionSerial completed_submission_serial_ = 0;
  SubmissionSerial last_issued_submission_serial_ = 0;
  DescriptorEpoch last_issued_descriptor_epoch_ = 0;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_SUBMISSION_LIFETIME_H_
