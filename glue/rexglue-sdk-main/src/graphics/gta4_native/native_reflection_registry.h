#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_REFLECTION_REGISTRY_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_REFLECTION_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <rex/graphics/gta4_native/title_commands.h>

namespace rex::graphics::gta4_native {

struct NativeReflectionTarget {
  ReflectionFamily family = ReflectionFamily::kMirror;
  ReflectionRole role = ReflectionRole::kColor;
  uint32_t wrapper = 0;
  uint32_t surface = 0;
  uint32_t texture = 0;
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t physical_width = 0;
  uint32_t physical_height = 0;
  uint32_t sample_count_override = 0;
};

// Reflection render attachments are transient capture workspaces. Their
// contents are valid only for the native frame in which the title rendered or
// cleared them. This is deliberately separate from the resolved reflection
// texture, whose last completed contents may be sampled by later frames.
struct NativeReflectionCaptureState {
  uint64_t epoch = 0;
  uint64_t write_serial = 0;
  uint32_t frame = 0;
  size_t command_index = SIZE_MAX;
  uint32_t render_phase = 0;
  uint8_t write_kind = 0;
};

inline bool HasCurrentNativeReflectionCaptureContent(
    const NativeReflectionCaptureState& state, uint32_t frame) {
  return state.epoch != 0 && state.write_serial != 0 &&
         state.frame == frame;
}

inline void ClaimNativeReflectionCaptureContent(
    NativeReflectionCaptureState& state, uint64_t epoch,
    uint64_t write_serial, uint32_t frame, size_t command_index,
    uint32_t render_phase, uint8_t write_kind) {
  state.epoch = epoch;
  state.write_serial = write_serial;
  state.frame = frame;
  state.command_index = command_index;
  state.render_phase = render_phase;
  state.write_kind = write_kind;
}

// Raising reflection resolution must not widen the title-visible LOD range.
// GTA authors and resolves the complete mip contract; native storage mirrors
// that count exactly at the higher physical resolution.
inline uint32_t GetNativeReflectionStorageMipLevelCount(
    const NativeReflectionTarget&, uint32_t guest_mip_levels) {
  return guest_mip_levels;
}

using NativeReflectionRegistry =
    std::unordered_map<uint32_t, NativeReflectionTarget>;

inline bool SameNativeReflectionRegistration(
    const NativeReflectionTarget& left,
    const NativeReflectionTarget& right) {
  return left.family == right.family && left.role == right.role &&
         left.wrapper == right.wrapper && left.surface == right.surface &&
         left.texture == right.texture &&
         left.logical_width == right.logical_width &&
         left.logical_height == right.logical_height &&
         left.physical_width == right.physical_width &&
         left.physical_height == right.physical_height &&
         left.sample_count_override == right.sample_count_override;
}

// A color reflection is exposed through an attachment surface and a sampled
// texture. Releasing either member retires both aliases. Keeping the other raw
// handle alive can misclassify a later title resource that reuses it.
inline size_t EraseNativeReflectionRegistration(
    NativeReflectionRegistry& registry, uint32_t resource) {
  const auto matched = registry.find(resource);
  if (matched == registry.end()) {
    return 0;
  }
  const NativeReflectionTarget registration = matched->second;
  return std::erase_if(registry, [&registration](const auto& entry) {
    return SameNativeReflectionRegistration(entry.second, registration);
  });
}

inline size_t RegisterNativeReflectionTarget(
    NativeReflectionRegistry& registry,
    const NativeReflectionTarget& registration) {
  size_t retired_aliases =
      EraseNativeReflectionRegistration(registry, registration.surface);
  if (registration.texture) {
    retired_aliases +=
        EraseNativeReflectionRegistration(registry, registration.texture);
  }
  registry[registration.surface] = registration;
  if (registration.texture) {
    registry[registration.texture] = registration;
  }
  return retired_aliases;
}

inline const NativeReflectionTarget* FindNativeReflectionSurface(
    const NativeReflectionRegistry& registry,
    const SurfaceDescriptor& descriptor, bool depth) {
  const auto entry = registry.find(descriptor.handle);
  if (entry == registry.end()) {
    return nullptr;
  }
  const NativeReflectionTarget& registration = entry->second;
  const ReflectionRole requested_role =
      depth ? ReflectionRole::kDepth : ReflectionRole::kColor;
  if (registration.surface != descriptor.handle ||
      registration.role != requested_role ||
      registration.logical_width != descriptor.width ||
      registration.logical_height != descriptor.height) {
    return nullptr;
  }
  return &registration;
}

inline const NativeReflectionTarget* FindNativeReflectionTexture(
    const NativeReflectionRegistry& registry, uint32_t handle,
    uint32_t logical_width, uint32_t logical_height) {
  const auto entry = registry.find(handle);
  if (entry == registry.end()) {
    return nullptr;
  }
  const NativeReflectionTarget& registration = entry->second;
  if (registration.role != ReflectionRole::kColor ||
      registration.texture != handle ||
      registration.logical_width != logical_width ||
      registration.logical_height != logical_height) {
    return nullptr;
  }
  return &registration;
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_REFLECTION_REGISTRY_H_
