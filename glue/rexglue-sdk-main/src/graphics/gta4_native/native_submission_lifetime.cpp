#include "native_submission_lifetime.h"

#include <algorithm>
#include <limits>

namespace rex::graphics::gta4_native {

bool NativeSubmissionLifetime::RegisterResource(ResourceKey key, bool descriptor_backed) {
  if (!key.identity && !key.generation) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return resources_.emplace(key, ResourceState{descriptor_backed}).second;
}

bool NativeSubmissionLifetime::QueueReference(ResourceKey key, uint64_t count) {
  if (!count) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto resource = resources_.find(key);
  if (resource == resources_.end() || resource->second.retirement_requested ||
      count > std::numeric_limits<uint64_t>::max() - resource->second.queued_reference_count) {
    return false;
  }
  resource->second.queued_reference_count += count;
  return true;
}

std::optional<NativeSubmissionLifetime::FrameToken> NativeSubmissionLifetime::BeginFrame(
    uint32_t slot_index) {
  std::lock_guard lock(mutex_);
  if (slot_index >= frame_slots_.size()) {
    return std::nullopt;
  }
  FrameSlot& slot = frame_slots_[slot_index];
  if (slot.state != FrameSlotState::kAvailable ||
      slot.generation == std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  ++slot.generation;
  slot.state = FrameSlotState::kRecording;
  slot.submission_serial = 0;
  slot.staged_references.clear();
  return FrameToken{slot_index, slot.generation};
}

bool NativeSubmissionLifetime::StageQueuedReference(FrameToken token, ResourceKey key,
                                                    uint64_t count) {
  if (!count) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  FrameSlot& slot = frame_slots_[token.slot];
  if (slot.state != FrameSlotState::kRecording) {
    return false;
  }
  const auto resource = resources_.find(key);
  if (resource == resources_.end()) {
    return false;
  }
  const auto total_staged = GetStagedReferenceCountLocked(key);
  if (!total_staged || *total_staged > resource->second.queued_reference_count ||
      count > resource->second.queued_reference_count - *total_staged) {
    return false;
  }
  uint64_t& staged_count = slot.staged_references[key];
  if (count > std::numeric_limits<uint64_t>::max() - staged_count) {
    if (!staged_count) {
      slot.staged_references.erase(key);
    }
    return false;
  }
  staged_count += count;
  return true;
}

std::optional<NativeSubmissionLifetime::SubmissionSerial>
NativeSubmissionLifetime::PrepareSubmission(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return std::nullopt;
  }
  FrameSlot& slot = frame_slots_[token.slot];
  if (slot.state != FrameSlotState::kRecording ||
      last_issued_submission_serial_ == std::numeric_limits<SubmissionSerial>::max()) {
    return std::nullopt;
  }
  ++last_issued_submission_serial_;
  slot.submission_serial = last_issued_submission_serial_;
  slot.state = FrameSlotState::kPrepared;
  return slot.submission_serial;
}

bool NativeSubmissionLifetime::CommitSubmission(FrameToken token, SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  FrameSlot& slot = frame_slots_[token.slot];
  if (slot.state != FrameSlotState::kPrepared || !serial || slot.submission_serial != serial) {
    return false;
  }

  // Validate the complete transaction before changing the first resource.
  for (const auto& [key, count] : slot.staged_references) {
    const auto resource = resources_.find(key);
    const auto total_staged = GetStagedReferenceCountLocked(key);
    if (resource == resources_.end() || !total_staged || *total_staged < count ||
        resource->second.queued_reference_count < *total_staged) {
      return false;
    }
  }
  for (const auto& [key, count] : slot.staged_references) {
    ResourceState& resource = resources_.find(key)->second;
    resource.queued_reference_count -= count;
    resource.last_use_serial = std::max(resource.last_use_serial, serial);
  }
  slot.staged_references.clear();
  slot.state = FrameSlotState::kSubmitted;
  return true;
}

bool NativeSubmissionLifetime::RollbackFrame(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  FrameSlot& slot = frame_slots_[token.slot];
  if (slot.state != FrameSlotState::kRecording && slot.state != FrameSlotState::kPrepared) {
    return false;
  }
  ResetSlotLocked(slot);
  return true;
}

bool NativeSubmissionLifetime::AbandonFrame(FrameToken token) {
  std::lock_guard lock(mutex_);
  if (!IsValidTokenLocked(token)) {
    return false;
  }
  FrameSlot& slot = frame_slots_[token.slot];
  if (slot.state != FrameSlotState::kRecording && slot.state != FrameSlotState::kPrepared) {
    return false;
  }
  for (const auto& [key, count] : slot.staged_references) {
    const auto resource = resources_.find(key);
    const auto total_staged = GetStagedReferenceCountLocked(key);
    if (resource == resources_.end() || !total_staged || *total_staged < count ||
        resource->second.queued_reference_count < *total_staged) {
      return false;
    }
  }
  for (const auto& [key, count] : slot.staged_references) {
    resources_.find(key)->second.queued_reference_count -= count;
  }
  ResetSlotLocked(slot);
  return true;
}

bool NativeSubmissionLifetime::NotifySlotFenceCompleted(uint32_t slot_index,
                                                        SubmissionSerial serial) {
  std::lock_guard lock(mutex_);
  if (slot_index >= frame_slots_.size()) {
    return false;
  }
  FrameSlot& slot = frame_slots_[slot_index];
  if (slot.state != FrameSlotState::kSubmitted || !serial || slot.submission_serial != serial) {
    return false;
  }
  completed_submission_serial_ = std::max(completed_submission_serial_, serial);
  ResetSlotLocked(slot);
  return true;
}

std::optional<NativeSubmissionLifetime::DescriptorEpoch>
NativeSubmissionLifetime::RequestRetirement(ResourceKey key) {
  std::lock_guard lock(mutex_);
  const auto resource = resources_.find(key);
  if (resource == resources_.end()) {
    return std::nullopt;
  }
  ResourceState& state = resource->second;
  if (state.retirement_requested) {
    return state.tombstone_epoch;
  }
  if (state.descriptor_backed) {
    if (last_issued_descriptor_epoch_ == std::numeric_limits<DescriptorEpoch>::max()) {
      return std::nullopt;
    }
    ++last_issued_descriptor_epoch_;
    state.tombstone_epoch = last_issued_descriptor_epoch_;
    descriptor_tombstones_.emplace(state.tombstone_epoch, key);
  }
  state.retirement_requested = true;
  return state.tombstone_epoch;
}

std::optional<std::vector<NativeSubmissionLifetime::DescriptorTombstone>>
NativeSubmissionLifetime::GetDescriptorTombstones(uint32_t slot_index,
                                                  DescriptorEpoch target_epoch) const {
  std::lock_guard lock(mutex_);
  if (!CanApplyDescriptorEpochLocked(slot_index, target_epoch)) {
    return std::nullopt;
  }
  const FrameSlot& slot = frame_slots_[slot_index];
  std::vector<DescriptorTombstone> result;
  auto tombstone = descriptor_tombstones_.upper_bound(slot.applied_descriptor_epoch);
  while (tombstone != descriptor_tombstones_.end() && tombstone->first <= target_epoch) {
    result.push_back({tombstone->second, tombstone->first});
    ++tombstone;
  }
  return result;
}

bool NativeSubmissionLifetime::CommitDescriptorTombstones(uint32_t slot_index,
                                                          DescriptorEpoch target_epoch) {
  std::lock_guard lock(mutex_);
  if (!CanApplyDescriptorEpochLocked(slot_index, target_epoch)) {
    return false;
  }
  frame_slots_[slot_index].applied_descriptor_epoch = target_epoch;
  return true;
}

bool NativeSubmissionLifetime::CanRetire(ResourceKey key) const {
  std::lock_guard lock(mutex_);
  const auto resource = resources_.find(key);
  return resource != resources_.end() && CanRetireLocked(resource->second);
}

bool NativeSubmissionLifetime::EraseRetiredResource(ResourceKey key) {
  std::lock_guard lock(mutex_);
  const auto resource = resources_.find(key);
  if (resource == resources_.end() || !CanRetireLocked(resource->second)) {
    return false;
  }
  if (resource->second.tombstone_epoch) {
    descriptor_tombstones_.erase(resource->second.tombstone_epoch);
  }
  resources_.erase(resource);
  return true;
}

std::optional<NativeSubmissionLifetime::FrameSlotSnapshot>
NativeSubmissionLifetime::GetFrameSlotSnapshot(uint32_t slot_index) const {
  std::lock_guard lock(mutex_);
  if (slot_index >= frame_slots_.size()) {
    return std::nullopt;
  }
  const FrameSlot& slot = frame_slots_[slot_index];
  uint64_t staged_reference_count = 0;
  for (const auto& [key, count] : slot.staged_references) {
    (void)key;
    if (count > std::numeric_limits<uint64_t>::max() - staged_reference_count) {
      staged_reference_count = std::numeric_limits<uint64_t>::max();
      break;
    }
    staged_reference_count += count;
  }
  return FrameSlotSnapshot{slot.state,
                           slot.generation,
                           slot.submission_serial,
                           slot.applied_descriptor_epoch,
                           slot.staged_references.size(),
                           staged_reference_count};
}

std::optional<NativeSubmissionLifetime::ResourceSnapshot>
NativeSubmissionLifetime::GetResourceSnapshot(ResourceKey key) const {
  std::lock_guard lock(mutex_);
  const auto resource = resources_.find(key);
  if (resource == resources_.end()) {
    return std::nullopt;
  }
  const ResourceState& state = resource->second;
  return ResourceSnapshot{state.descriptor_backed, state.retirement_requested,
                          state.queued_reference_count, state.last_use_serial,
                          state.tombstone_epoch};
}

NativeSubmissionLifetime::SubmissionSerial NativeSubmissionLifetime::completed_submission_serial()
    const {
  std::lock_guard lock(mutex_);
  return completed_submission_serial_;
}

NativeSubmissionLifetime::SubmissionSerial NativeSubmissionLifetime::last_issued_submission_serial()
    const {
  std::lock_guard lock(mutex_);
  return last_issued_submission_serial_;
}

NativeSubmissionLifetime::DescriptorEpoch NativeSubmissionLifetime::last_issued_descriptor_epoch()
    const {
  std::lock_guard lock(mutex_);
  return last_issued_descriptor_epoch_;
}

bool NativeSubmissionLifetime::IsValidTokenLocked(FrameToken token) const {
  return token.slot < frame_slots_.size() && token.generation &&
         frame_slots_[token.slot].generation == token.generation;
}

std::optional<uint64_t> NativeSubmissionLifetime::GetStagedReferenceCountLocked(
    ResourceKey key) const {
  uint64_t total = 0;
  for (const FrameSlot& slot : frame_slots_) {
    const auto staged = slot.staged_references.find(key);
    if (staged == slot.staged_references.end()) {
      continue;
    }
    if (staged->second > std::numeric_limits<uint64_t>::max() - total) {
      return std::nullopt;
    }
    total += staged->second;
  }
  return total;
}

bool NativeSubmissionLifetime::CanApplyDescriptorEpochLocked(uint32_t slot_index,
                                                             DescriptorEpoch target_epoch) const {
  if (slot_index >= frame_slots_.size()) {
    return false;
  }
  const FrameSlot& slot = frame_slots_[slot_index];
  if (slot.state != FrameSlotState::kAvailable || target_epoch < slot.applied_descriptor_epoch ||
      target_epoch > last_issued_descriptor_epoch_) {
    return false;
  }
  auto tombstone = descriptor_tombstones_.upper_bound(slot.applied_descriptor_epoch);
  while (tombstone != descriptor_tombstones_.end() && tombstone->first <= target_epoch) {
    const auto resource = resources_.find(tombstone->second);
    if (resource == resources_.end() || resource->second.queued_reference_count ||
        (resource->second.last_use_serial &&
         resource->second.last_use_serial > completed_submission_serial_)) {
      return false;
    }
    ++tombstone;
  }
  return true;
}

bool NativeSubmissionLifetime::CanRetireLocked(const ResourceState& resource) const {
  if (!resource.retirement_requested || resource.queued_reference_count ||
      (resource.last_use_serial && resource.last_use_serial > completed_submission_serial_)) {
    return false;
  }
  if (!resource.descriptor_backed) {
    return true;
  }
  if (!resource.tombstone_epoch) {
    return false;
  }
  return std::all_of(frame_slots_.begin(), frame_slots_.end(), [&resource](const FrameSlot& slot) {
    return slot.applied_descriptor_epoch >= resource.tombstone_epoch;
  });
}

void NativeSubmissionLifetime::ResetSlotLocked(FrameSlot& slot) {
  slot.state = FrameSlotState::kAvailable;
  slot.submission_serial = 0;
  slot.staged_references.clear();
}

}  // namespace rex::graphics::gta4_native
