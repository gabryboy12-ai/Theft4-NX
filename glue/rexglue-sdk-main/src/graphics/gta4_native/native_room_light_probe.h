#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <rex/graphics/gta4_native/lighting_semantics.h>

namespace rex::graphics::gta4_native {

inline constexpr uint32_t kNativeRoomLightInputGridAxis = 16;
inline constexpr uint32_t kNativeRoomLightInputSampleStride = 16;

struct NativeRoomLightInputGridPoint {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t offset = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Shared by Vulkan copy recording and artifact coordinates. Block-compressed
// images retain a complete raw block at each selected point, not decoded RGBA.
constexpr NativeRoomLightInputGridPoint NativeRoomLightInputPoint(
    uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t block_extent = 1) {
  if (!width || !height || x >= kNativeRoomLightInputGridAxis ||
      y >= kNativeRoomLightInputGridAxis || (block_extent != 1 && block_extent != 4)) {
    return {};
  }
  const uint32_t point_x = uint32_t((uint64_t(x) * width) / kNativeRoomLightInputGridAxis +
                                    width / (kNativeRoomLightInputGridAxis * 2));
  const uint32_t point_y = uint32_t((uint64_t(y) * height) / kNativeRoomLightInputGridAxis +
                                    height / (kNativeRoomLightInputGridAxis * 2));
  const uint32_t copy_x = block_extent == 4 ? point_x & ~3u : point_x;
  const uint32_t copy_y = block_extent == 4 ? point_y & ~3u : point_y;
  const uint32_t available_width = width - copy_x;
  const uint32_t available_height = height - copy_y;
  return {copy_x, copy_y, (y * kNativeRoomLightInputGridAxis + x) * kNativeRoomLightInputSampleStride,
           available_width < block_extent ? available_width : block_extent,
           available_height < block_extent ? available_height : block_extent};
}

constexpr auto NativeRoomLightProbeScopeWords(const LightingContext& context,
                                             uint32_t render_phase, uint32_t target,
                                             uint32_t depth,
                                             const std::array<uint32_t, 6>& viewport) {
  // Intentionally exclude frame-local view/occurrence IDs and pooled addresses.
  return std::array{uint32_t(context.stage), uint32_t(context.source), context.source_function,
                    render_phase, target, depth,
                    viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]};
}

// A physical light may issue different contribution variants. Neither a reused
// guest pointer nor a frame-local occurrence alone identifies this work item.
struct NativeRoomLightProbeKey {
  uint64_t instance = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  uint32_t selector = UINT32_MAX;
  uint32_t mode = UINT32_MAX;
  // Logical output/executor/viewport signature, not a pooled guest view pointer.
  uint64_t view_scope = 0;
  uint32_t effective_mode = UINT32_MAX;
  auto operator<=>(const NativeRoomLightProbeKey&) const = default;
};

class NativeRoomLightProbeQueue {
 public:
  static constexpr std::size_t kCapacity = 256;
  static constexpr uint32_t kAttemptLimit = 3;
  enum class Observation { kAdded, kExisting, kRejected };
  struct Entry {
    uint32_t first_seen_frame = 0;
    uint32_t last_seen_frame = 0;
    uint64_t sightings = 0;
    uint32_t attempts = 0;
    bool readback_complete = false;
    bool in_flight = false;
  };

  Observation Observe(const NativeRoomLightProbeKey& key, uint32_t frame) {
    if (!key.instance || !key.vertex_shader || !key.pixel_shader) {
      return Observation::kRejected;
    }
    auto found = entries_.find(key);
    if (found != entries_.end()) {
      if (found->second.last_seen_frame != frame) {
        ++found->second.sightings;
      }
      found->second.last_seen_frame = frame;
      return Observation::kExisting;
    }
    if (entries_.size() >= kCapacity) {
      return Observation::kRejected;
    }
    entries_.emplace(key, Entry{frame, frame, 1});
    return Observation::kAdded;
  }

  std::optional<NativeRoomLightProbeKey> Next(uint32_t frame,
                                             uint32_t priority_selector = UINT32_MAX) const {
    std::optional<NativeRoomLightProbeKey> result;
    uint32_t fewest_attempts = kAttemptLimit;
    for (const auto& [key, entry] : entries_) {
      if (entry.last_seen_frame != frame || entry.readback_complete || entry.in_flight ||
          entry.attempts >= kAttemptLimit) {
        continue;
      }
      const bool priority = priority_selector != UINT32_MAX && key.selector == priority_selector;
      const bool previous_priority = result && priority_selector != UINT32_MAX &&
                                     result->selector == priority_selector;
      if (!result || (priority && !previous_priority) ||
          (priority == previous_priority && entry.attempts < fewest_attempts)) {
        result = key;
        fewest_attempts = entry.attempts;
      }
    }
    return result;
  }

  bool Begin(const NativeRoomLightProbeKey& key) {
    auto found = entries_.find(key);
    if (found == entries_.end() || found->second.in_flight || found->second.readback_complete ||
        found->second.attempts >= kAttemptLimit) {
      return false;
    }
    found->second.in_flight = true;
    return true;
  }

  bool Finish(const NativeRoomLightProbeKey& key, bool readback_complete) {
    auto found = entries_.find(key);
    if (found == entries_.end() || !found->second.in_flight || found->second.readback_complete ||
        found->second.attempts >= kAttemptLimit) {
      return false;
    }
    ++found->second.attempts;
    found->second.in_flight = false;
    found->second.readback_complete = readback_complete;
    return true;
  }

  const auto& entries() const { return entries_; }

 private:
  std::map<NativeRoomLightProbeKey, Entry> entries_;
};

// Owned by an exact selected attempt, not by a recyclable GPU readback slot.
// A part is successful only after its final evidence has been published. Slot
// consumption may precede that publication by many frames.
struct NativeRoomLightProbeReceipt {
  enum class Part { kColor, kStencil, kQuery, kInputs, kCount };
  enum class State { kNotStarted, kPending, kFailed, kSucceeded };
  const NativeRoomLightProbeKey key;
  const uint32_t frame;
  const size_t setup_command;
  const size_t contribution_command;
  const bool stencil_required;
  const bool inputs_required;

  NativeRoomLightProbeReceipt(NativeRoomLightProbeKey selected_key, uint32_t selected_frame,
                              size_t setup, size_t contribution, bool stencil, bool inputs = false)
      : key(selected_key), frame(selected_frame), setup_command(setup),
        contribution_command(contribution), stencil_required(stencil), inputs_required(inputs) {
    if (!stencil_required) {
      states_[size_t(Part::kStencil)].store(State::kSucceeded);
    }
    if (!inputs_required) {
      states_[size_t(Part::kInputs)].store(State::kSucceeded);
    }
  }

  void Begin(Part part) {
    State expected = State::kNotStarted;
    states_[size_t(part)].compare_exchange_strong(expected, State::kPending);
  }
  void Complete(Part part, bool success) {
    State expected = State::kPending;
    states_[size_t(part)].compare_exchange_strong(
        expected, success ? State::kSucceeded : State::kFailed, std::memory_order_acq_rel);
  }
  State Get(Part part) const { return states_[size_t(part)].load(std::memory_order_acquire); }
  void FailUnstarted() {
    // Called only after this attempt's GPU payloads and queries were consumed.
    // Unsupported/missing copies must be explicit failures, never an endless drain.
    for (auto& state : states_) {
      State expected = State::kNotStarted;
      state.compare_exchange_strong(expected, State::kFailed);
    }
  }
  bool Terminal() const {
    for (const auto& state : states_) {
      const auto value = state.load(std::memory_order_acquire);
      if (value == State::kNotStarted || value == State::kPending) {
        return false;
      }
    }
    return true;
  }
  bool Succeeded(Part part) const { return Get(part) == State::kSucceeded; }
  bool Succeeded() const {
    for (const auto& state : states_) {
      if (state.load(std::memory_order_acquire) != State::kSucceeded) {
        return false;
      }
    }
    return true;
  }

 private:
  std::array<std::atomic<State>, size_t(Part::kCount)> states_{};
};

// Construct inside the analysis callback. All early returns and exceptions
// terminally fail the receipt; explicit success follows artifact flush + logs.
class NativeRoomLightProbeCompletion {
 public:
  NativeRoomLightProbeCompletion(std::shared_ptr<NativeRoomLightProbeReceipt> receipt,
                                 NativeRoomLightProbeReceipt::Part part)
      : receipt_(std::move(receipt)), part_(part) {}
  ~NativeRoomLightProbeCompletion() {
    if (receipt_) {
      receipt_->Complete(part_, success_);
    }
  }
  void SetSuccess(bool success) { success_ = success; }
  // Hand an in-progress receipt to another owner without publishing it yet.
  void Release() { receipt_.reset(); }

 private:
  std::shared_ptr<NativeRoomLightProbeReceipt> receipt_;
  NativeRoomLightProbeReceipt::Part part_;
  bool success_ = false;
};

}  // namespace rex::graphics::gta4_native
