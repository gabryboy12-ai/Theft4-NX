#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_FRAME_CONTEXT_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_FRAME_CONTEXT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace rex::graphics::gta4_native {

// CPU-side ownership contract for the native renderer's two frame slots.
//
// This class deliberately has no Vulkan dependency. The integrating renderer
// owns the actual command pools, fences, query pools, readback allocations and
// images; this class proves when a slot-local object may be touched and keeps
// shared image-layout bookkeeping transactional until queue submission has
// succeeded.
class NativeFrameContextRing final {
 public:
  using SubmissionSerial = uint64_t;
  using Layout = uint32_t;

  static constexpr size_t kSlotCount = 2;

  struct ObjectKey {
    uint64_t identity = 0;
    uint64_t generation = 0;

    constexpr bool operator==(const ObjectKey&) const = default;
    constexpr bool operator<(const ObjectKey& other) const {
      return identity < other.identity ||
             (identity == other.identity && generation < other.generation);
    }
  };

  struct FrameToken {
    uint32_t slot = UINT32_MAX;
    uint64_t generation = 0;

    constexpr bool operator==(const FrameToken&) const = default;
  };

  // Query pools and their host-visible result storage are permanent properties
  // of a configured slot. Their identities must be nonzero and unique across
  // both slots. Slots that do not use GPU queries may remain unconfigured.
  struct QueryReadbackResources {
    uint64_t query_pool = 0;
    uint64_t readback = 0;

    constexpr bool operator==(const QueryReadbackResources&) const = default;
  };

  enum class SlotState : uint8_t {
    kAvailable,
    kRecording,
    kPrepared,
    kSubmitted,
    kWaiting,
    kReadyForReset,
  };

  struct SlotSnapshot {
    SlotState state = SlotState::kAvailable;
    uint64_t generation = 0;
    SubmissionSerial submission_serial = 0;
    bool submission_committed = false;
    bool query_readback_claimed = false;
    bool query_readback_acknowledged = false;
    size_t referenced_object_count = 0;
    size_t deferred_destruction_count = 0;
    size_t layout_prediction_count = 0;
  };

  // Slot resources are configured after the renderer creates the real Vulkan
  // objects and stay owned by that slot until renderer teardown. This prevents
  // a query pool or readback allocation from silently moving to another
  // in-flight frame. Release requires the exact resources and an available
  // slot so stale teardown cannot detach a later configuration.
  bool ConfigureSlot(uint32_t slot, QueryReadbackResources resources);
  bool ReleaseSlot(uint32_t slot, QueryReadbackResources resources);
  std::optional<QueryReadbackResources> GetSlotResources(uint32_t slot) const;

  // Only one slot may be in the CPU recording/prepared transaction at a time.
  // The other slot may remain submitted to the GPU. A generation-bearing token
  // prevents stale callbacks from mutating a later use of the same slot.
  std::optional<FrameToken> BeginFrame(uint32_t slot);
  std::optional<SubmissionSerial> PrepareSubmission(FrameToken token);
  bool CommitSubmission(FrameToken token, SubmissionSerial serial);

  // A failed recording or queue submission rolls back every shared-layout
  // prediction atomically. Deferred destruction requests remain journaled and
  // become immediately eligible for the caller to execute.
  bool RollbackFrame(FrameToken token);

  // The explicit wait states mirror the renderer sequence: begin waiting for
  // the exact slot fence, report success (or cancel a failed wait), consume
  // query/readback results and destruction work, then reset the slot.
  bool BeginWait(FrameToken token, SubmissionSerial serial);
  bool CancelWait(FrameToken token, SubmissionSerial serial);
  bool CompleteWait(FrameToken token, SubmissionSerial serial);
  bool ResetSlot(FrameToken token);

  // Claiming authorizes reset/write of this slot's query pool and readback
  // allocation while recording. Results cannot be obtained or acknowledged
  // until the exact submitted slot has completed its wait.
  std::optional<QueryReadbackResources> ClaimQueryReadback(FrameToken token);
  std::optional<QueryReadbackResources> GetCompletedQueryReadback(FrameToken token) const;
  bool AcknowledgeQueryReadback(FrameToken token);

  // Generic object-use tracking rejects a use after destruction was deferred
  // and rejects reuse after the caller acknowledged actual destruction.
  bool MarkObjectUse(FrameToken token, ObjectKey object);
  bool DeferDestruction(FrameToken token, ObjectKey object);

  // Journals are transactional: reading one does not consume it. Reset is
  // illegal until the caller acknowledges the whole journal, so a partial host
  // destruction failure cannot lose the remaining work.
  std::optional<std::vector<ObjectKey>> GetDeferredDestructionJournal(FrameToken token) const;
  bool AcknowledgeDeferredDestructionJournal(FrameToken token);
  bool WasDestroyed(ObjectKey object) const;

  // Shared layouts represent the ordered queue tail, not physical completion.
  // Predictions are visible to the next frame only after CommitSubmission.
  // Before that point they belong to the sole recording transaction and can be
  // restored exactly by RollbackFrame.
  bool RegisterSharedLayout(ObjectKey object, Layout initial_layout);
  std::optional<Layout> GetPredictedSharedLayout(ObjectKey object) const;
  bool PredictSharedLayout(FrameToken token, ObjectKey object, Layout expected_layout,
                           Layout predicted_layout);

  std::optional<SlotSnapshot> GetSlotSnapshot(uint32_t slot) const;
  SubmissionSerial last_issued_submission_serial() const;
  SubmissionSerial completed_submission_serial() const;

 private:
  struct LayoutPrediction {
    Layout before = 0;
    Layout after = 0;
  };

  struct Slot {
    SlotState state = SlotState::kAvailable;
    bool configured = false;
    QueryReadbackResources resources{};
    uint64_t generation = 0;
    SubmissionSerial submission_serial = 0;
    bool submission_committed = false;
    bool query_readback_claimed = false;
    bool query_readback_acknowledged = false;
    std::set<ObjectKey> referenced_objects;
    std::vector<ObjectKey> deferred_destructions;
    std::map<ObjectKey, LayoutPrediction> layout_predictions;
  };

  static bool IsValidObject(ObjectKey object);
  bool IsValidTokenLocked(FrameToken token) const;
  bool IsRecordingTokenLocked(FrameToken token) const;
  bool IsObjectUsableLocked(ObjectKey object) const;
  bool CanResetSlotLocked(const Slot& slot) const;
  void ClearReusableStateLocked(Slot& slot);

  mutable std::mutex mutex_;
  std::array<Slot, kSlotCount> slots_{};
  std::optional<uint32_t> recording_slot_;
  std::map<ObjectKey, Layout> shared_layouts_;
  std::map<ObjectKey, FrameToken> pending_destructions_;
  std::set<ObjectKey> destroyed_objects_;
  SubmissionSerial last_issued_submission_serial_ = 0;
  SubmissionSerial completed_submission_serial_ = 0;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_FRAME_CONTEXT_H_
