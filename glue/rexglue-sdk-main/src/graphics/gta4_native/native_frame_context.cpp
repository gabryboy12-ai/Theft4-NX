#include "native_frame_context.h"

#include <algorithm>
#include <limits>

namespace rex::graphics::gta4_native {

bool NativeFrameContextRing::ConfigureSlot(uint32_t slot_index, QueryReadbackResources resources) {
  if (!resources.query_pool || !resources.readback || resources.query_pool == resources.readback) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (slot_index >= slots_.size() || slots_[slot_index].configured ||
      slots_[slot_index].state != SlotState::kAvailable) {
    return false;
  }

  for (const Slot& slot : slots_) {
    if (!slot.configured) {
      continue;
    }
    if (slot.resources.query_pool == resources.query_pool ||
        slot.resources.query_pool == resources.readback ||
        slot.resources.readback == resources.query_pool ||
        slot.resources.readback == resources.readback) {
      return false;
    }
  }

  slots_[slot_index].configured = true;
  slots_[slot_index].resources = resources;
  return true;
}

bool NativeFrameContextRing::ReleaseSlot(uint32_t slot_index,
                                         QueryReadbackResources resources) {
  std::lock_guard lock(mutex_);
  if (slot_index >= slots_.size()) {
    return false;
  }
  Slot& slot = slots_[slot_index];
  if (!slot.configured || slot.state != SlotState::kAvailable ||
      slot.resources != resources) {
    return false;
  }
  slot.configured = false;
  slot.resources = {};
  return true;
}

std::optional<NativeFrameContextRing::QueryReadbackResources>
NativeFrameContextRing::GetSlotResources(uint32_t slot_index) const {
  std::lock_guard lock(mutex_);
  if (slot_index >= slots_.size() || !slots_[slot_index].configured) {
    return std::nullopt;
  }
  return slots_[slot_index].resources;
}

std::optional<NativeFrameContextRing::FrameToken> NativeFrameContextRing::BeginFrame(
    uint32_t slot_index) {
  std::lock_guard lock(mutex_);
  if (slot_index >= slots_.size() || recording_slot_) {
    return std::nullopt;
  }

  Slot& slot = slots_[slot_index];
  if (slot.state != SlotState::kAvailable ||
      slot.generation == std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }

  ++slot.generation;
  slot.state = SlotState::kRecording;
  ClearReusableStateLocked(slot);
  recording_slot_ = slot_index;
  return FrameToken{slot_index, slot.generation};
}

std::optional<NativeFrameContextRing::SubmissionSerial> NativeFrameContextRing::PrepareSubmission(
    FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsRecordingTokenLocked(token) ||
      last_issued_submission_serial_ == std::numeric_limits<SubmissionSerial>::max()) {
    return std::nullopt;
  }

  Slot& slot = slots_[token.slot];
  ++last_issued_submission_serial_;
  slot.submission_serial = last_issued_submission_serial_;
  slot.state = SlotState::kPrepared;
  return slot.submission_serial;
}

bool NativeFrameContextRing::CommitSubmission(FrameToken token, SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }

  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kPrepared || !serial || slot.submission_serial != serial ||
      recording_slot_ != token.slot) {
    return false;
  }

  // Predictions already describe the ordered queue tail. Queue acceptance is
  // their commit point, so only the rollback journal needs to stop being live.
  slot.state = SlotState::kSubmitted;
  slot.submission_committed = true;
  recording_slot_.reset();
  return true;
}

bool NativeFrameContextRing::RollbackFrame(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }

  Slot& slot = slots_[token.slot];
  if ((slot.state != SlotState::kRecording && slot.state != SlotState::kPrepared) ||
      recording_slot_ != token.slot) {
    return false;
  }

  // Validate the entire rollback before changing the first global layout.
  for (const auto& [object, prediction] : slot.layout_predictions) {
    const auto layout = shared_layouts_.find(object);
    if (layout == shared_layouts_.end() || layout->second != prediction.after) {
      return false;
    }
  }
  for (const auto& [object, prediction] : slot.layout_predictions) {
    shared_layouts_.find(object)->second = prediction.before;
  }

  slot.state = SlotState::kReadyForReset;
  slot.submission_committed = false;
  recording_slot_.reset();
  return true;
}

bool NativeFrameContextRing::BeginWait(FrameToken token, SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kSubmitted || !serial || slot.submission_serial != serial ||
      !slot.submission_committed) {
    return false;
  }
  slot.state = SlotState::kWaiting;
  return true;
}

bool NativeFrameContextRing::CancelWait(FrameToken token, SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kWaiting || !serial || slot.submission_serial != serial) {
    return false;
  }
  slot.state = SlotState::kSubmitted;
  return true;
}

bool NativeFrameContextRing::CompleteWait(FrameToken token, SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kWaiting || !serial || slot.submission_serial != serial) {
    return false;
  }
  completed_submission_serial_ = std::max(completed_submission_serial_, serial);
  slot.state = SlotState::kReadyForReset;
  return true;
}

bool NativeFrameContextRing::ResetSlot(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (!CanResetSlotLocked(slot)) {
    return false;
  }
  slot.state = SlotState::kAvailable;
  ClearReusableStateLocked(slot);
  return true;
}

std::optional<NativeFrameContextRing::QueryReadbackResources>
NativeFrameContextRing::ClaimQueryReadback(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsRecordingTokenLocked(token)) {
    return std::nullopt;
  }
  Slot& slot = slots_[token.slot];
  if (!slot.configured || slot.query_readback_claimed) {
    return std::nullopt;
  }
  slot.query_readback_claimed = true;
  slot.query_readback_acknowledged = false;
  return slot.resources;
}

std::optional<NativeFrameContextRing::QueryReadbackResources>
NativeFrameContextRing::GetCompletedQueryReadback(FrameToken token) const {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return std::nullopt;
  }
  const Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kReadyForReset || !slot.submission_committed ||
      !slot.query_readback_claimed || slot.query_readback_acknowledged) {
    return std::nullopt;
  }
  return slot.resources;
}

bool NativeFrameContextRing::AcknowledgeQueryReadback(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kReadyForReset || !slot.submission_committed ||
      !slot.query_readback_claimed || slot.query_readback_acknowledged) {
    return false;
  }
  slot.query_readback_acknowledged = true;
  return true;
}

bool NativeFrameContextRing::MarkObjectUse(FrameToken token, ObjectKey object) {
  if (!IsValidObject(object)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!IsRecordingTokenLocked(token) || !IsObjectUsableLocked(object)) {
    return false;
  }
  slots_[token.slot].referenced_objects.insert(object);
  return true;
}

bool NativeFrameContextRing::DeferDestruction(FrameToken token, ObjectKey object) {
  if (!IsValidObject(object)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!IsRecordingTokenLocked(token) || destroyed_objects_.contains(object) ||
      pending_destructions_.contains(object)) {
    return false;
  }
  slots_[token.slot].deferred_destructions.push_back(object);
  pending_destructions_.emplace(object, token);
  return true;
}

std::optional<std::vector<NativeFrameContextRing::ObjectKey>>
NativeFrameContextRing::GetDeferredDestructionJournal(FrameToken token) const {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token) || slots_[token.slot].state != SlotState::kReadyForReset) {
    return std::nullopt;
  }
  return slots_[token.slot].deferred_destructions;
}

bool NativeFrameContextRing::AcknowledgeDeferredDestructionJournal(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  Slot& slot = slots_[token.slot];
  if (slot.state != SlotState::kReadyForReset) {
    return false;
  }

  // Validate the complete journal before consuming any entry.
  for (ObjectKey object : slot.deferred_destructions) {
    const auto pending = pending_destructions_.find(object);
    if (pending == pending_destructions_.end() || pending->second != token ||
        destroyed_objects_.contains(object)) {
      return false;
    }
  }
  for (ObjectKey object : slot.deferred_destructions) {
    pending_destructions_.erase(object);
    shared_layouts_.erase(object);
    destroyed_objects_.insert(object);
  }
  slot.deferred_destructions.clear();
  return true;
}

bool NativeFrameContextRing::WasDestroyed(ObjectKey object) const {
  if (!IsValidObject(object)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return destroyed_objects_.contains(object);
}

bool NativeFrameContextRing::RegisterSharedLayout(ObjectKey object, Layout initial_layout) {
  if (!IsValidObject(object)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (destroyed_objects_.contains(object) || pending_destructions_.contains(object)) {
    return false;
  }
  return shared_layouts_.emplace(object, initial_layout).second;
}

std::optional<NativeFrameContextRing::Layout> NativeFrameContextRing::GetPredictedSharedLayout(
    ObjectKey object) const {
  if (!IsValidObject(object)) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const auto layout = shared_layouts_.find(object);
  if (layout == shared_layouts_.end()) {
    return std::nullopt;
  }
  return layout->second;
}

bool NativeFrameContextRing::PredictSharedLayout(FrameToken token, ObjectKey object,
                                                 Layout expected_layout, Layout predicted_layout) {
  if (!IsValidObject(object)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!IsRecordingTokenLocked(token) || !IsObjectUsableLocked(object)) {
    return false;
  }

  const auto layout = shared_layouts_.find(object);
  if (layout == shared_layouts_.end() || layout->second != expected_layout) {
    return false;
  }

  Slot& slot = slots_[token.slot];
  slot.referenced_objects.insert(object);
  if (expected_layout == predicted_layout) {
    return true;
  }

  auto [prediction, inserted] = slot.layout_predictions.try_emplace(
      object, LayoutPrediction{expected_layout, predicted_layout});
  if (!inserted) {
    prediction->second.after = predicted_layout;
  }
  layout->second = predicted_layout;
  return true;
}

std::optional<NativeFrameContextRing::SlotSnapshot> NativeFrameContextRing::GetSlotSnapshot(
    uint32_t slot_index) const {
  std::lock_guard lock(mutex_);
  if (slot_index >= slots_.size()) {
    return std::nullopt;
  }
  const Slot& slot = slots_[slot_index];
  return SlotSnapshot{slot.state,
                      slot.generation,
                      slot.submission_serial,
                      slot.submission_committed,
                      slot.query_readback_claimed,
                      slot.query_readback_acknowledged,
                      slot.referenced_objects.size(),
                      slot.deferred_destructions.size(),
                      slot.layout_predictions.size()};
}

NativeFrameContextRing::SubmissionSerial NativeFrameContextRing::last_issued_submission_serial()
    const {
  std::lock_guard lock(mutex_);
  return last_issued_submission_serial_;
}

NativeFrameContextRing::SubmissionSerial NativeFrameContextRing::completed_submission_serial()
    const {
  std::lock_guard lock(mutex_);
  return completed_submission_serial_;
}

bool NativeFrameContextRing::IsValidObject(ObjectKey object) {
  return object.identity || object.generation;
}

bool NativeFrameContextRing::IsValidTokenLocked(FrameToken token) const {
  return token.slot < slots_.size() && token.generation &&
         slots_[token.slot].generation == token.generation;
}

bool NativeFrameContextRing::IsRecordingTokenLocked(FrameToken token) const {
  return IsValidTokenLocked(token) && recording_slot_ == token.slot &&
         slots_[token.slot].state == SlotState::kRecording;
}

bool NativeFrameContextRing::IsObjectUsableLocked(ObjectKey object) const {
  if (destroyed_objects_.contains(object)) {
    return false;
  }
  return !pending_destructions_.contains(object);
}

bool NativeFrameContextRing::CanResetSlotLocked(const Slot& slot) const {
  if (slot.state != SlotState::kReadyForReset || !slot.deferred_destructions.empty()) {
    return false;
  }
  return !slot.submission_committed || !slot.query_readback_claimed ||
         slot.query_readback_acknowledged;
}

void NativeFrameContextRing::ClearReusableStateLocked(Slot& slot) {
  slot.submission_serial = 0;
  slot.submission_committed = false;
  slot.query_readback_claimed = false;
  slot.query_readback_acknowledged = false;
  slot.referenced_objects.clear();
  slot.deferred_destructions.clear();
  slot.layout_predictions.clear();
}

}  // namespace rex::graphics::gta4_native
