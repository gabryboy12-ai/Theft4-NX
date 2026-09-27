#include "native_descriptor_backend.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rex::graphics::gta4_native {
namespace {

NativeDescriptorStatus EvaluateIndexedBackend(const NativeDescriptorPolicyInputs& inputs) {
  const auto& features = inputs.indexing_features;
  // GTA IV's generated table indices are dynamically uniform. Requiring the
  // non-uniform-indexing feature bits would reject a supported layout without
  // providing any correctness benefit.
  if (!features.runtime_descriptor_array || !features.descriptor_binding_partially_bound) {
    return NativeDescriptorStatus::kMissingIndexingFeatures;
  }
  if (inputs.use_update_after_bind_layout &&
      (!features.descriptor_binding_sampled_image_update_after_bind ||
       !features.descriptor_binding_sampler_update_after_bind ||
       !features.descriptor_binding_update_unused_while_pending)) {
    return NativeDescriptorStatus::kMissingUpdateAfterBindFeatures;
  }

  const auto& limits = inputs.limits;
  if (inputs.required_sampled_images > limits.max_per_stage_sampled_images ||
      inputs.required_sampled_images > limits.max_descriptor_set_sampled_images ||
      inputs.required_samplers > limits.max_per_stage_samplers ||
      inputs.required_samplers > limits.max_descriptor_set_samplers ||
      inputs.required_per_stage_resources > limits.max_per_stage_resources ||
      inputs.required_pool_descriptors > limits.max_descriptors_in_all_pools) {
    return NativeDescriptorStatus::kInsufficientDeviceLimits;
  }

  const auto& layout = inputs.layout_support;
  if (!layout.queried) {
    return NativeDescriptorStatus::kLayoutSupportNotQueried;
  }
  if (!layout.supported || inputs.required_sampled_images > layout.supported_sampled_images ||
      inputs.required_samplers > layout.supported_samplers) {
    return NativeDescriptorStatus::kLayoutUnsupported;
  }
  return NativeDescriptorStatus::kSuccess;
}

bool WritesMatch(const NativeDescriptorWrite& left, const NativeDescriptorWrite& right) {
  return left.slot == right.slot && left.generation == right.generation &&
         left.epoch == right.epoch && left.payload == right.payload &&
         left.tombstone == right.tombstone;
}

uint32_t SearchLargestSupportedLayout(NativeDescriptorKind kind, uint32_t upper_bound,
                                      const NativeDescriptorFixedLayoutProbe& probe,
                                      uint32_t* probe_count) {
  uint32_t supported = 0;
  uint32_t unsupported = upper_bound;
  while (supported < unsupported) {
    const uint32_t remaining = unsupported - supported;
    const uint32_t candidate = supported + remaining / 2 + remaining % 2;
    if (probe_count) {
      ++*probe_count;
    }
    if (probe(kind, candidate)) {
      supported = candidate;
    } else {
      unsupported = candidate - 1;
    }
  }
  return supported;
}

}  // namespace

NativeDescriptorCapacity ChooseNativeDescriptorCapacity(
    const NativeDescriptorCapacityInputs& inputs) {
  NativeDescriptorCapacity result{};
  if (!inputs.desired_sampled_images_per_set || !inputs.desired_samplers ||
      !inputs.minimum_sampled_images_per_set || !inputs.minimum_samplers ||
      inputs.minimum_sampled_images_per_set > inputs.desired_sampled_images_per_set ||
      inputs.minimum_samplers > inputs.desired_samplers || !inputs.sampled_image_set_count ||
      !inputs.frame_copy_count || !inputs.storage_descriptors_per_copy) {
    return result;
  }

  const uint64_t image_set_count = inputs.sampled_image_set_count;
  const uint64_t frame_copy_count = inputs.frame_copy_count;
  const uint64_t storage_count = inputs.storage_descriptors_per_copy;
  const auto& limits = inputs.limits;

  uint64_t image_capacity = std::min<uint64_t>(
      {inputs.desired_sampled_images_per_set,
       uint64_t(limits.max_per_stage_sampled_images) / image_set_count,
       uint64_t(limits.max_descriptor_set_sampled_images) / image_set_count});
  const uint64_t independent_sampler_capacity =
      std::min<uint64_t>({inputs.desired_samplers, limits.max_per_stage_samplers,
                          limits.max_descriptor_set_samplers});

  const uint64_t pool_budget_per_copy =
      uint64_t(limits.max_descriptors_in_all_pools) / frame_copy_count;
  const uint64_t combined_budget =
      std::min<uint64_t>(limits.max_per_stage_resources, pool_budget_per_copy);
  const uint64_t minimum_fixed_budget = storage_count + inputs.minimum_samplers;
  if (combined_budget < minimum_fixed_budget) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }

  image_capacity =
      std::min(image_capacity, (combined_budget - minimum_fixed_budget) / image_set_count);
  if (image_capacity < inputs.minimum_sampled_images_per_set) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }

  const uint64_t sampled_images_per_stage = image_capacity * image_set_count;
  const uint64_t sampler_budget = combined_budget - storage_count - sampled_images_per_stage;
  const uint64_t sampler_capacity = std::min(independent_sampler_capacity, sampler_budget);
  if (sampler_capacity < inputs.minimum_samplers) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }

  const uint64_t resources_per_stage = sampled_images_per_stage + sampler_capacity + storage_count;
  const uint64_t pool_sampled_images = sampled_images_per_stage * frame_copy_count;
  const uint64_t pool_samplers = sampler_capacity * frame_copy_count;
  const uint64_t pool_storage_buffers = storage_count * frame_copy_count;
  const uint64_t pool_descriptors = pool_sampled_images + pool_samplers + pool_storage_buffers;
  const uint64_t uint32_max = std::numeric_limits<uint32_t>::max();
  if (image_capacity > uint32_max || sampler_capacity > uint32_max ||
      sampled_images_per_stage > uint32_max || resources_per_stage > uint32_max ||
      pool_sampled_images > uint32_max || pool_samplers > uint32_max ||
      pool_storage_buffers > uint32_max || pool_descriptors > uint32_max) {
    result.status = NativeDescriptorStatus::kInvalidConfiguration;
    return result;
  }

  result.status = NativeDescriptorStatus::kSuccess;
  result.sampled_images_per_set = uint32_t(image_capacity);
  result.samplers = uint32_t(sampler_capacity);
  result.sampled_images_per_stage = uint32_t(sampled_images_per_stage);
  result.resources_per_stage = uint32_t(resources_per_stage);
  result.pool_sampled_images = uint32_t(pool_sampled_images);
  result.pool_samplers = uint32_t(pool_samplers);
  result.pool_storage_buffers = uint32_t(pool_storage_buffers);
  result.pool_descriptors = uint32_t(pool_descriptors);
  return result;
}

NativeDescriptorPageCapacity NegotiateNativeDescriptorPageCapacity(
    const NativeDescriptorPageCapacityInputs& inputs,
    const NativeDescriptorFixedLayoutProbe& probe) {
  NativeDescriptorPageCapacity result{};
  if (!inputs.desired_sampled_images_per_set || !inputs.desired_samplers ||
      !inputs.minimum_sampled_images_per_set || !inputs.minimum_samplers ||
      inputs.minimum_sampled_images_per_set > inputs.desired_sampled_images_per_set ||
      inputs.minimum_samplers > inputs.desired_samplers || !inputs.sampled_image_set_count ||
      !inputs.frame_copy_count || !inputs.storage_descriptors_per_copy) {
    return result;
  }
  if (!probe) {
    result.status = NativeDescriptorStatus::kLayoutSupportNotQueried;
    return result;
  }

  const uint64_t image_set_count = inputs.sampled_image_set_count;
  const uint64_t frame_copy_count = inputs.frame_copy_count;
  const uint64_t storage_count = inputs.storage_descriptors_per_copy;
  const NativeDescriptorLimits& limits = inputs.limits;
  if (inputs.pool_storage_descriptors >= limits.max_descriptors_in_all_pools) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }
  const uint64_t available_pool_descriptors =
      uint64_t(limits.max_descriptors_in_all_pools) -
      inputs.pool_storage_descriptors;
  const uint64_t pool_budget_per_copy =
      available_pool_descriptors / frame_copy_count;
  const uint64_t per_stage_budget = limits.max_per_stage_resources;
  const uint64_t minimum_per_stage_fixed_budget =
      storage_count + inputs.minimum_samplers;
  if (per_stage_budget < minimum_per_stage_fixed_budget ||
      pool_budget_per_copy < inputs.minimum_samplers) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }

  uint64_t image_upper = std::min<uint64_t>(
      {inputs.desired_sampled_images_per_set,
       uint64_t(limits.max_per_stage_sampled_images) / image_set_count,
       uint64_t(limits.max_descriptor_set_sampled_images) / image_set_count,
       (per_stage_budget - minimum_per_stage_fixed_budget) / image_set_count,
       (pool_budget_per_copy - inputs.minimum_samplers) / image_set_count});
  if (image_upper < inputs.minimum_sampled_images_per_set) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }
  const uint32_t image_capacity = SearchLargestSupportedLayout(
      NativeDescriptorKind::kSampledImage, uint32_t(image_upper), probe,
      &result.sampled_image_probe_count);
  if (image_capacity < inputs.minimum_sampled_images_per_set) {
    result.status = NativeDescriptorStatus::kLayoutUnsupported;
    return result;
  }

  const uint64_t sampled_images_per_stage = uint64_t(image_capacity) * image_set_count;
  const uint64_t per_stage_sampler_budget =
      per_stage_budget - storage_count - sampled_images_per_stage;
  const uint64_t pool_sampler_budget =
      pool_budget_per_copy - sampled_images_per_stage;
  const uint64_t sampler_upper =
      std::min<uint64_t>({inputs.desired_samplers, limits.max_per_stage_samplers,
                          limits.max_descriptor_set_samplers,
                          per_stage_sampler_budget, pool_sampler_budget});
  if (sampler_upper < inputs.minimum_samplers) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }
  const uint32_t sampler_capacity = SearchLargestSupportedLayout(
      NativeDescriptorKind::kSampler, uint32_t(sampler_upper), probe,
      &result.sampler_probe_count);
  if (sampler_capacity < inputs.minimum_samplers) {
    result.status = NativeDescriptorStatus::kLayoutUnsupported;
    return result;
  }

  const uint64_t resources_per_stage =
      sampled_images_per_stage + sampler_capacity + storage_count;
  const uint64_t descriptors_per_copy = sampled_images_per_stage + sampler_capacity;
  const uint64_t descriptors_per_page = descriptors_per_copy * frame_copy_count;
  if (!descriptors_per_page ||
      descriptors_per_page > available_pool_descriptors) {
    result.status = NativeDescriptorStatus::kInsufficientDeviceLimits;
    return result;
  }
  const uint64_t maximum_page_count =
      available_pool_descriptors / descriptors_per_page;
  const uint64_t required_pool_descriptors =
      descriptors_per_page + inputs.pool_storage_descriptors;
  const uint64_t uint32_max = std::numeric_limits<uint32_t>::max();
  if (sampled_images_per_stage > uint32_max || resources_per_stage > uint32_max ||
      descriptors_per_copy > uint32_max || descriptors_per_page > uint32_max ||
      required_pool_descriptors > uint32_max ||
      maximum_page_count > uint32_max) {
    result.status = NativeDescriptorStatus::kInvalidConfiguration;
    return result;
  }

  result.status = NativeDescriptorStatus::kSuccess;
  result.sampled_images_per_set = image_capacity;
  result.samplers = sampler_capacity;
  result.sampled_images_per_stage = uint32_t(sampled_images_per_stage);
  result.resources_per_stage = uint32_t(resources_per_stage);
  result.descriptors_per_copy = uint32_t(descriptors_per_copy);
  result.descriptors_per_page = uint32_t(descriptors_per_page);
  result.required_pool_descriptors = uint32_t(required_pool_descriptors);
  result.maximum_page_count = uint32_t(maximum_page_count);
  return result;
}

NativeDescriptorPolicyDecision ChooseNativeDescriptorBackend(
    const NativeDescriptorPolicyInputs& inputs) {
  NativeDescriptorPolicyDecision decision{};
  decision.backend = NativeDescriptorBackend::kCached;

  if (!inputs.required_sampled_images || !inputs.required_samplers ||
      !inputs.required_per_stage_resources || !inputs.required_pool_descriptors) {
    decision.status = NativeDescriptorStatus::kInvalidConfiguration;
    return decision;
  }
  if (inputs.request == NativeDescriptorBackendRequest::kCached) {
    decision.status = NativeDescriptorStatus::kSuccess;
    return decision;
  }

  const NativeDescriptorStatus indexed_status = EvaluateIndexedBackend(inputs);
  if (indexed_status == NativeDescriptorStatus::kSuccess) {
    decision.status = NativeDescriptorStatus::kSuccess;
    decision.backend = NativeDescriptorBackend::kIndexed;
    return decision;
  }

  if (inputs.request == NativeDescriptorBackendRequest::kIndexed) {
    decision.status = indexed_status;
    decision.indexed_rejection = indexed_status;
    return decision;
  }

  decision.status = NativeDescriptorStatus::kSuccess;
  decision.backend = NativeDescriptorBackend::kCached;
  decision.indexed_rejection = indexed_status;
  return decision;
}

NativeDescriptorEpochTable::NativeDescriptorEpochTable(const NativeDescriptorEpochConfig& config)
    : kind_(config.kind), fallback_(config.fallback) {
  if (!config.frame_copy_count || !config.slot_capacity) {
    initialization_status_ = NativeDescriptorStatus::kInvalidConfiguration;
    return;
  }
  if (!fallback_.valid(kind_)) {
    initialization_status_ = NativeDescriptorStatus::kInvalidDescriptor;
    return;
  }

  const size_t frame_count = config.frame_copy_count;
  const size_t slot_count = config.slot_capacity;
  if (slot_count > std::numeric_limits<size_t>::max() / frame_count) {
    initialization_status_ = NativeDescriptorStatus::kInvalidConfiguration;
    return;
  }

  slots_.resize(slot_count);
  applied_epochs_.resize(frame_count * slot_count);
  dirty_slots_by_copy_.resize(frame_count);
  dirty_slot_membership_.resize(frame_count * slot_count);
  frame_copy_submission_serials_.resize(frame_count);
  pending_batches_.resize(frame_count);
  for (uint32_t slot = 0; slot < config.slot_capacity; ++slot) {
    slots_[slot].payload = fallback_;
    free_slots_.push_back(slot);
  }
  initialization_status_ = NativeDescriptorStatus::kSuccess;
}

bool NativeDescriptorEpochTable::HasPendingBatch() const {
  return std::any_of(pending_batches_.begin(), pending_batches_.end(),
                     [](const PendingBatch& batch) { return batch.id != 0; });
}

NativeDescriptorStatus NativeDescriptorEpochTable::ValidateLiveHandle(
    NativeDescriptorSlotHandle handle) const {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (handle.index >= slots_.size() || !handle.generation) {
    return NativeDescriptorStatus::kInvalidSlot;
  }
  const Slot& slot = slots_[handle.index];
  if (slot.generation != handle.generation) {
    return NativeDescriptorStatus::kStaleSlot;
  }
  if (slot.state != SlotState::kLive) {
    return NativeDescriptorStatus::kSlotNotLive;
  }
  return NativeDescriptorStatus::kSuccess;
}

bool NativeDescriptorEpochTable::AdvanceEpoch(uint64_t* next_epoch) {
  if (!next_epoch || current_epoch_ == std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  ++current_epoch_;
  *next_epoch = current_epoch_;
  return true;
}

size_t NativeDescriptorEpochTable::AppliedEpochOffset(uint32_t frame_copy, uint32_t slot) const {
  return static_cast<size_t>(frame_copy) * slots_.size() + slot;
}

void NativeDescriptorEpochTable::MarkSlotDirty(uint32_t slot) {
  for (uint32_t frame_copy = 0; frame_copy < dirty_slots_by_copy_.size(); ++frame_copy) {
    const size_t membership_offset = AppliedEpochOffset(frame_copy, slot);
    if (dirty_slot_membership_[membership_offset]) {
      continue;
    }
    dirty_slot_membership_[membership_offset] = 1;
    dirty_slots_by_copy_[frame_copy].push_back(slot);
  }
}

NativeDescriptorAllocation NativeDescriptorEpochTable::Allocate(
    const NativeDescriptorPayload& payload) {
  NativeDescriptorAllocation result{};
  if (!valid()) {
    result.status = NativeDescriptorStatus::kInvalidConfiguration;
    return result;
  }
  if (!payload.valid(kind_)) {
    result.status = NativeDescriptorStatus::kInvalidDescriptor;
    return result;
  }
  if (HasPendingBatch()) {
    result.status = NativeDescriptorStatus::kPendingWriteBatch;
    return result;
  }

  ReclaimReadySlots();
  if (free_slots_.empty()) {
    result.status = NativeDescriptorStatus::kCapacityExhausted;
    return result;
  }

  const uint32_t slot_index = free_slots_.front();
  Slot& slot = slots_[slot_index];
  if (slot.generation == std::numeric_limits<uint64_t>::max()) {
    result.status = NativeDescriptorStatus::kCounterExhausted;
    return result;
  }

  uint64_t epoch = 0;
  if (!AdvanceEpoch(&epoch)) {
    result.status = NativeDescriptorStatus::kCounterExhausted;
    return result;
  }

  free_slots_.pop_front();
  ++slot.generation;
  slot.state = SlotState::kLive;
  slot.epoch = epoch;
  slot.tombstone_epoch = 0;
  slot.retire_after_gpu_serial = 0;
  slot.payload = payload;
  ++live_count_;
  MarkSlotDirty(slot_index);

  result.status = NativeDescriptorStatus::kSuccess;
  result.handle = {slot_index, slot.generation};
  return result;
}

NativeDescriptorStatus NativeDescriptorEpochTable::Update(NativeDescriptorSlotHandle handle,
                                                          const NativeDescriptorPayload& payload) {
  if (!payload.valid(kind_)) {
    return NativeDescriptorStatus::kInvalidDescriptor;
  }
  if (HasPendingBatch()) {
    return NativeDescriptorStatus::kPendingWriteBatch;
  }
  const NativeDescriptorStatus handle_status = ValidateLiveHandle(handle);
  if (handle_status != NativeDescriptorStatus::kSuccess) {
    return handle_status;
  }

  uint64_t epoch = 0;
  if (!AdvanceEpoch(&epoch)) {
    return NativeDescriptorStatus::kCounterExhausted;
  }
  Slot& slot = slots_[handle.index];
  slot.payload = payload;
  slot.epoch = epoch;
  MarkSlotDirty(handle.index);
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeDescriptorEpochTable::Retire(NativeDescriptorSlotHandle handle,
                                                          uint64_t last_use_gpu_serial) {
  if (HasPendingBatch()) {
    return NativeDescriptorStatus::kPendingWriteBatch;
  }
  const NativeDescriptorStatus handle_status = ValidateLiveHandle(handle);
  if (handle_status != NativeDescriptorStatus::kSuccess) {
    return handle_status;
  }

  uint64_t epoch = 0;
  if (!AdvanceEpoch(&epoch)) {
    return NativeDescriptorStatus::kCounterExhausted;
  }
  Slot& slot = slots_[handle.index];
  slot.state = SlotState::kRetiring;
  slot.payload = fallback_;
  slot.epoch = epoch;
  slot.tombstone_epoch = epoch;
  slot.retire_after_gpu_serial = last_use_gpu_serial;
  --live_count_;
  ++retiring_count_;
  retiring_slots_.push_back(handle.index);
  MarkSlotDirty(handle.index);
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorWriteBatch NativeDescriptorEpochTable::BeginFrameCopyWrites(uint32_t frame_copy) {
  NativeDescriptorWriteBatch result{};
  result.frame_copy = frame_copy;
  if (!valid()) {
    result.status = NativeDescriptorStatus::kInvalidConfiguration;
    return result;
  }
  if (frame_copy >= frame_copy_submission_serials_.size()) {
    result.status = NativeDescriptorStatus::kInvalidFrameCopy;
    return result;
  }
  if (pending_batches_[frame_copy].id) {
    result.status = NativeDescriptorStatus::kPendingWriteBatch;
    return result;
  }
  if (frame_copy_submission_serials_[frame_copy] > completed_gpu_serial_) {
    result.status = NativeDescriptorStatus::kFrameCopyInUse;
    return result;
  }
  if (next_batch_id_ == std::numeric_limits<uint64_t>::max()) {
    result.status = NativeDescriptorStatus::kCounterExhausted;
    return result;
  }

  ++next_batch_id_;
  result.status = NativeDescriptorStatus::kSuccess;
  result.batch_id = next_batch_id_;
  const std::vector<uint32_t>& dirty_slots = dirty_slots_by_copy_[frame_copy];
  result.writes.reserve(dirty_slots.size());
  for (uint32_t slot_index : dirty_slots) {
    const Slot& slot = slots_[slot_index];
    if (slot.state == SlotState::kFree ||
        applied_epochs_[AppliedEpochOffset(frame_copy, slot_index)] >= slot.epoch) {
      continue;
    }
    result.writes.push_back({slot_index, slot.generation, slot.epoch, slot.payload,
                             slot.state == SlotState::kRetiring});
  }

  PendingBatch& pending = pending_batches_[frame_copy];
  pending.id = result.batch_id;
  pending.writes = result.writes;
  return result;
}

NativeDescriptorStatus NativeDescriptorEpochTable::CommitFrameCopyWrites(
    const NativeDescriptorWriteBatch& batch) {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (batch.frame_copy >= pending_batches_.size()) {
    return NativeDescriptorStatus::kInvalidFrameCopy;
  }
  PendingBatch& pending = pending_batches_[batch.frame_copy];
  if (!batch.batch_id || pending.id != batch.batch_id ||
      batch.writes.size() != pending.writes.size()) {
    return NativeDescriptorStatus::kInvalidWriteBatch;
  }
  for (size_t index = 0; index < pending.writes.size(); ++index) {
    if (!WritesMatch(batch.writes[index], pending.writes[index])) {
      return NativeDescriptorStatus::kInvalidWriteBatch;
    }
  }
  for (const NativeDescriptorWrite& write : pending.writes) {
    applied_epochs_[AppliedEpochOffset(batch.frame_copy, write.slot)] = write.epoch;
  }
  std::vector<uint32_t>& dirty_slots = dirty_slots_by_copy_[batch.frame_copy];
  size_t retained_dirty_count = 0;
  for (uint32_t slot_index : dirty_slots) {
    const Slot& slot = slots_[slot_index];
    if (slot.state != SlotState::kFree &&
        applied_epochs_[AppliedEpochOffset(batch.frame_copy, slot_index)] < slot.epoch) {
      dirty_slots[retained_dirty_count++] = slot_index;
      continue;
    }
    dirty_slot_membership_[AppliedEpochOffset(batch.frame_copy, slot_index)] = 0;
  }
  dirty_slots.resize(retained_dirty_count);
  pending = {};
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeDescriptorEpochTable::CancelFrameCopyWrites(
    const NativeDescriptorWriteBatch& batch) {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (batch.frame_copy >= pending_batches_.size()) {
    return NativeDescriptorStatus::kInvalidFrameCopy;
  }
  PendingBatch& pending = pending_batches_[batch.frame_copy];
  if (!batch.batch_id || pending.id != batch.batch_id) {
    return NativeDescriptorStatus::kInvalidWriteBatch;
  }
  pending = {};
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeDescriptorEpochTable::MarkFrameCopySubmitted(uint32_t frame_copy,
                                                                          uint64_t gpu_serial) {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (frame_copy >= frame_copy_submission_serials_.size()) {
    return NativeDescriptorStatus::kInvalidFrameCopy;
  }
  if (pending_batches_[frame_copy].id) {
    return NativeDescriptorStatus::kPendingWriteBatch;
  }
  if (frame_copy_submission_serials_[frame_copy] > completed_gpu_serial_) {
    return NativeDescriptorStatus::kFrameCopyInUse;
  }
  if (!gpu_serial || gpu_serial <= greatest_submitted_gpu_serial_) {
    return NativeDescriptorStatus::kNonMonotonicGpuSerial;
  }
  frame_copy_submission_serials_[frame_copy] = gpu_serial;
  greatest_submitted_gpu_serial_ = gpu_serial;
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeDescriptorEpochTable::MarkGpuCompleted(uint64_t gpu_serial) {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (gpu_serial < completed_gpu_serial_) {
    return NativeDescriptorStatus::kNonMonotonicGpuSerial;
  }
  completed_gpu_serial_ = gpu_serial;
  return NativeDescriptorStatus::kSuccess;
}

size_t NativeDescriptorEpochTable::ReclaimReadySlots() {
  if (!valid() || HasPendingBatch()) {
    return 0;
  }

  size_t reclaimed = 0;
  size_t retained_retiring_count = 0;
  for (uint32_t slot_index : retiring_slots_) {
    Slot& slot = slots_[slot_index];
    if (slot.state != SlotState::kRetiring) {
      continue;
    }
    if (completed_gpu_serial_ < slot.retire_after_gpu_serial) {
      retiring_slots_[retained_retiring_count++] = slot_index;
      continue;
    }

    bool tombstone_applied_to_every_copy = true;
    for (uint32_t frame_copy = 0; frame_copy < frame_copy_submission_serials_.size();
         ++frame_copy) {
      if (applied_epochs_[AppliedEpochOffset(frame_copy, slot_index)] < slot.tombstone_epoch) {
        tombstone_applied_to_every_copy = false;
        break;
      }
    }
    if (!tombstone_applied_to_every_copy) {
      retiring_slots_[retained_retiring_count++] = slot_index;
      continue;
    }

    slot.state = SlotState::kFree;
    slot.payload = fallback_;
    slot.tombstone_epoch = 0;
    slot.retire_after_gpu_serial = 0;
    free_slots_.push_back(slot_index);
    --retiring_count_;
    ++reclaimed;
  }
  retiring_slots_.resize(retained_retiring_count);
  return reclaimed;
}

bool NativeDescriptorEpochTable::IsLive(NativeDescriptorSlotHandle handle) const {
  return ValidateLiveHandle(handle) == NativeDescriptorStatus::kSuccess;
}

bool NativeDescriptorEpochTable::IsRetirementComplete(NativeDescriptorSlotHandle handle) const {
  if (!valid() || handle.index >= slots_.size() || !handle.generation) {
    return false;
  }
  const Slot& slot = slots_[handle.index];
  if (slot.generation != handle.generation) {
    return true;
  }
  return slot.state == SlotState::kFree;
}

NativeStableDescriptorSlotTable::NativeStableDescriptorSlotTable(
    const NativeStableDescriptorSlotConfig& config)
    : page_(config.page), slots_(config.slot_capacity) {
  if (!config.slot_capacity) {
    return;
  }
  for (uint32_t slot = 0; slot < config.slot_capacity; ++slot) {
    free_slots_.push_back(slot);
  }
  initialization_status_ = NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeStableDescriptorSlotTable::ValidateHandle(
    NativeDescriptorSlotHandle handle) const {
  if (!valid()) {
    return NativeDescriptorStatus::kInvalidConfiguration;
  }
  if (handle.page != page_ || handle.index >= slots_.size() || !handle.generation) {
    return NativeDescriptorStatus::kInvalidSlot;
  }
  if (slots_[handle.index].generation != handle.generation) {
    return NativeDescriptorStatus::kStaleSlot;
  }
  return NativeDescriptorStatus::kSuccess;
}

bool NativeStableDescriptorSlotTable::AdvanceSemanticEpoch() {
  if (semantic_epoch_ == std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  ++semantic_epoch_;
  return true;
}

NativeDescriptorAllocation NativeStableDescriptorSlotTable::Allocate() {
  NativeDescriptorAllocation result{};
  ++counters_.allocation_calls;
  if (!valid()) {
    return result;
  }
  if (free_slots_.empty()) {
    result.status = NativeDescriptorStatus::kCapacityExhausted;
    return result;
  }

  const uint32_t slot_index = free_slots_.front();
  Slot& slot = slots_[slot_index];
  if (slot.generation == std::numeric_limits<uint64_t>::max() ||
      !AdvanceSemanticEpoch()) {
    result.status = NativeDescriptorStatus::kCounterExhausted;
    return result;
  }

  free_slots_.pop_front();
  ++slot.generation;
  slot.state = SlotState::kLive;
  slot.retire_after_gpu_serial = 0;
  ++live_count_;
  ++counters_.publish_requirements;

  result.status = NativeDescriptorStatus::kSuccess;
  result.handle = {slot_index, slot.generation, page_};
  return result;
}

NativeDescriptorStatus NativeStableDescriptorSlotTable::AbortUnsubmitted(
    NativeDescriptorSlotHandle handle) {
  ++counters_.abort_calls;
  const NativeDescriptorStatus handle_status = ValidateHandle(handle);
  if (handle_status != NativeDescriptorStatus::kSuccess) {
    return handle_status;
  }
  Slot& slot = slots_[handle.index];
  if (slot.state != SlotState::kLive) {
    return NativeDescriptorStatus::kSlotNotLive;
  }
  if (!AdvanceSemanticEpoch()) {
    return NativeDescriptorStatus::kCounterExhausted;
  }

  slot.state = SlotState::kFree;
  slot.retire_after_gpu_serial = 0;
  free_slots_.push_back(handle.index);
  --live_count_;
  ++counters_.aborted_unsubmitted_slots;
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeStableDescriptorSlotTable::Retire(
    NativeDescriptorSlotHandle handle, uint64_t last_use_gpu_serial) {
  ++counters_.retirement_calls;
  const NativeDescriptorStatus handle_status = ValidateHandle(handle);
  if (handle_status != NativeDescriptorStatus::kSuccess) {
    return handle_status;
  }
  Slot& slot = slots_[handle.index];
  if (slot.state != SlotState::kLive) {
    return NativeDescriptorStatus::kSlotNotLive;
  }

  slot.state = SlotState::kRetiring;
  slot.retire_after_gpu_serial = last_use_gpu_serial;
  --live_count_;
  ++retiring_count_;
  return NativeDescriptorStatus::kSuccess;
}

NativeDescriptorStatus NativeStableDescriptorSlotTable::Reclaim(
    NativeDescriptorSlotHandle handle, uint64_t completed_gpu_serial,
    const ClearCallback& clear_callback) {
  ++counters_.reclaim_calls;
  const NativeDescriptorStatus handle_status = ValidateHandle(handle);
  if (handle_status != NativeDescriptorStatus::kSuccess) {
    return handle_status;
  }
  Slot& slot = slots_[handle.index];
  if (slot.state != SlotState::kRetiring) {
    return NativeDescriptorStatus::kSlotNotLive;
  }
  if (completed_gpu_serial < slot.retire_after_gpu_serial) {
    return NativeDescriptorStatus::kGpuSubmissionPending;
  }
  if (!clear_callback ||
      semantic_epoch_ == std::numeric_limits<uint64_t>::max()) {
    return !clear_callback ? NativeDescriptorStatus::kDescriptorClearFailed
                           : NativeDescriptorStatus::kCounterExhausted;
  }

  ++counters_.clear_callbacks;
  if (!clear_callback(handle)) {
    return NativeDescriptorStatus::kDescriptorClearFailed;
  }
  if (!AdvanceSemanticEpoch()) {
    return NativeDescriptorStatus::kCounterExhausted;
  }

  slot.state = SlotState::kFree;
  slot.retire_after_gpu_serial = 0;
  free_slots_.push_back(handle.index);
  --retiring_count_;
  ++counters_.reclaimed_slots;
  return NativeDescriptorStatus::kSuccess;
}

bool NativeStableDescriptorSlotTable::IsLive(
    NativeDescriptorSlotHandle handle) const {
  return ValidateHandle(handle) == NativeDescriptorStatus::kSuccess &&
         slots_[handle.index].state == SlotState::kLive;
}

bool NativeStableDescriptorSlotTable::IsRetiring(
    NativeDescriptorSlotHandle handle) const {
  return ValidateHandle(handle) == NativeDescriptorStatus::kSuccess &&
         slots_[handle.index].state == SlotState::kRetiring;
}

bool NativeStableDescriptorSlotTable::IsRetirementComplete(
    NativeDescriptorSlotHandle handle) const {
  if (!valid() || handle.page != page_ || handle.index >= slots_.size() ||
      !handle.generation) {
    return false;
  }
  const Slot& slot = slots_[handle.index];
  return slot.generation != handle.generation || slot.state == SlotState::kFree;
}
}  // namespace rex::graphics::gta4_native
