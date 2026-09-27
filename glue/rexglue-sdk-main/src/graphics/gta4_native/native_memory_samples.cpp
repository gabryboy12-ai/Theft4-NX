#include "native_memory_samples.h"

#include <algorithm>
#include <limits>

namespace rex::graphics::gta4_native::memory {

namespace {

template <typename Enum>
constexpr size_t EnumIndex(Enum value) {
  return size_t(value);
}

int64_t SaturatingSignedDelta(uint64_t current, uint64_t previous) {
  constexpr uint64_t kMaximumSigned = uint64_t(std::numeric_limits<int64_t>::max());
  if (current >= previous) {
    return int64_t(std::min(current - previous, kMaximumSigned));
  }
  return -int64_t(std::min(previous - current, kMaximumSigned));
}

}  // namespace

void SnapshotSeries::Begin(size_t capacity) {
  Clear();
  samples_.resize(std::max<size_t>(1, std::min(capacity, kMaximumSamples)));
}

void SnapshotSeries::Clear() {
  samples_.clear();
  last_snapshot_ = {};
  peaks_ = {};
  growth_ = {};
  shrink_ = {};
  head_ = 0;
  size_ = 0;
  next_sample_index_ = 0;
  overwritten_samples_ = 0;
  has_last_snapshot_ = false;
}

void SnapshotSeries::Push(Snapshot snapshot) {
  if (samples_.empty()) {
    Begin();
  }
  const bool overwriting = size_ == samples_.size();
  if (overwriting) {
    ++overwritten_samples_;
  }
  snapshot.sample_index = next_sample_index_++;
  for (size_t index = 0; index < kCategoryCount; ++index) {
    CategoryUsage& usage = snapshot.categories[index];
    peaks_[index] = std::max(peaks_[index], usage.live_bytes);
    if (has_last_snapshot_) {
      const uint64_t previous = last_snapshot_.categories[index].live_bytes;
      if (usage.live_bytes >= previous) {
        growth_[index] += usage.live_bytes - previous;
      } else {
        shrink_[index] += previous - usage.live_bytes;
      }
    }
    usage.peak_bytes = peaks_[index];
    usage.cumulative_growth_bytes = growth_[index];
    usage.cumulative_shrink_bytes = shrink_[index];
  }
  snapshot.dropped_samples = overwritten_samples_;
  samples_[head_] = snapshot;
  head_ = (head_ + 1) % samples_.size();
  if (size_ < samples_.size()) {
    ++size_;
  }
  last_snapshot_ = snapshot;
  has_last_snapshot_ = true;
}

bool SnapshotSeries::CopyOldest(size_t offset, Snapshot* snapshot) const {
  if (!snapshot || offset >= size_ || samples_.empty()) {
    return false;
  }
  const size_t oldest = (head_ + samples_.size() - size_) % samples_.size();
  *snapshot = samples_[(oldest + offset) % samples_.size()];
  return true;
}

void LifecycleEventSeries::Begin(size_t capacity) {
  Clear();
  events_.resize(std::max<size_t>(1, std::min(capacity, kMaximumLifecycleEvents)));
}

void LifecycleEventSeries::Clear() {
  events_.clear();
  head_ = 0;
  size_ = 0;
  next_event_index_ = 0;
  overwritten_events_ = 0;
}

void LifecycleEventSeries::Push(LifecycleEvent event) {
  if (events_.empty()) {
    Begin();
  }
  if (size_ == events_.size()) {
    ++overwritten_events_;
  }
  event.event_index = next_event_index_++;
  events_[head_] = event;
  head_ = (head_ + 1) % events_.size();
  if (size_ < events_.size()) {
    ++size_;
  }
}

bool LifecycleEventSeries::CopyOldest(size_t offset, LifecycleEvent* event) const {
  if (!event || offset >= size_ || events_.empty()) {
    return false;
  }
  const size_t oldest = (head_ + events_.size() - size_) % events_.size();
  *event = events_[(oldest + offset) % events_.size()];
  return true;
}

const char* CategoryName(Category category) {
  static constexpr std::array<const char*, kCategoryCount> kNames = {
      "host-texture-payloads",
      "host-texture-metadata",
      "host-buffer-payloads",
      "host-vertex-conversions",
      "host-index-conversions",
      "host-command-transport",
      "host-device-snapshots",
      "host-shaders-and-declarations",
      "host-pipeline-and-sampler-metadata",
      "host-surface-and-placement-metadata",
      "host-profiler",
      "gpu-texture-images",
      "gpu-reflection-texture-images",
      "gpu-color-surfaces",
      "gpu-depth-surfaces",
      "gpu-reflection-surfaces",
      "gpu-depth-handoff-scratch",
      "gpu-postfx-scene",
      "gpu-postfx-split",
      "gpu-sun-shafts",
      "gpu-smaa-extent",
      "gpu-smaa-lookup",
      "gpu-smaa-staging",
      "gpu-upload",
      "gpu-persistent-geometry",
      "gpu-readback-and-probes",
      "gpu-null-resources",
  };
  const size_t index = EnumIndex(category);
  return index < kNames.size() ? kNames[index] : "unknown";
}

const char* ProcessVmCategoryName(ProcessVmCategory category) {
  static constexpr std::array<const char*, kProcessVmCategoryCount> kNames = {
      "malloc-small",  "malloc-large",     "malloc-tiny", "malloc-nano",
      "malloc-medium", "malloc-other",     "iosurface",   "iokit",
      "stack",         "untagged-private", "shared",      "other",
  };
  const size_t index = EnumIndex(category);
  return index < kNames.size() ? kNames[index] : "unknown";
}

const char* ResourceKindName(ResourceKind kind) {
  static constexpr std::array<const char*, size_t(ResourceKind::kCount)> kNames = {
      "buffer",          "vertex-conversion", "index-conversion", "texture-resource",
      "texture-image",   "color-surface",     "depth-surface",    "reflection-surface",
      "pipeline",        "sampler",           "descriptor-pool",  "command-pool",
      "upload-buffer",   "persistent-buffer",
  };
  const size_t index = EnumIndex(kind);
  return index < kNames.size() ? kNames[index] : "unknown";
}

const char* LifecycleActionName(LifecycleAction action) {
  static constexpr std::array<const char*, size_t(LifecycleAction::kCount)> kNames = {
      "baseline", "create", "replace", "release-requested", "retire-pending",
      "evict",    "destroy", "pool-reset", "pool-grow",
  };
  const size_t index = EnumIndex(action);
  return index < kNames.size() ? kNames[index] : "unknown";
}

const char* LifecycleReasonName(LifecycleReason reason) {
  static constexpr std::array<const char*, size_t(LifecycleReason::kCount)> kNames = {
      "none",          "cache-miss",       "content-changed", "guest-release",
      "superseded",    "unused",           "budget-pressure", "allocation-recovery",
      "capacity-growth", "frame-reuse",    "shutdown",
  };
  const size_t index = EnumIndex(reason);
  return index < kNames.size() ? kNames[index] : "unknown";
}

bool CategoryIsGpu(Category category) {
  return EnumIndex(category) >= EnumIndex(Category::kGpuTextureImages) &&
         EnumIndex(category) < kCategoryCount;
}

uint64_t SumLiveBytes(const Snapshot& snapshot, bool gpu) {
  uint64_t result = 0;
  for (size_t index = 0; index < kCategoryCount; ++index) {
    if (CategoryIsGpu(Category(index)) == gpu) {
      result += snapshot.categories[index].live_bytes;
    }
  }
  return result;
}

std::vector<DeltaEvent> BuildDeltaEvents(const std::vector<Snapshot>& samples) {
  std::vector<DeltaEvent> events;
  if (samples.size() < 2) {
    return events;
  }
  for (size_t sample_index = 1; sample_index < samples.size(); ++sample_index) {
    const Snapshot& previous = samples[sample_index - 1];
    const Snapshot& current = samples[sample_index];
    for (size_t category_index = 0; category_index < kCategoryCount; ++category_index) {
      const CategoryUsage& previous_usage = previous.categories[category_index];
      const CategoryUsage& current_usage = current.categories[category_index];
      const int64_t byte_delta =
          SaturatingSignedDelta(current_usage.live_bytes, previous_usage.live_bytes);
      const int64_t count_delta =
          SaturatingSignedDelta(current_usage.live_count, previous_usage.live_count);
      if (!byte_delta && !count_delta) {
        continue;
      }
      events.push_back({current.sample_index, current.submitted_frame, current.marker,
                        Category(category_index), byte_delta, count_delta});
    }
  }
  return events;
}

}  // namespace rex::graphics::gta4_native::memory
