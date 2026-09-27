#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

enum class NativeBindingRealization : uint8_t {
  kUnused,
  kGuestNull,
  kSnapshotMissing,
  kImageFailed,
  kSamplerFailed,
  kReady,
};

constexpr NativeBindingRealization SelectNativeBindingRealization(
    bool required, bool guest_nonnull, bool snapshot, bool image, bool sampler) {
  if (!required) {
    return NativeBindingRealization::kUnused;
  }
  if (!guest_nonnull && !snapshot) {
    return NativeBindingRealization::kGuestNull;
  }
  if (!snapshot) {
    return NativeBindingRealization::kSnapshotMissing;
  }
  if (!image) {
    return NativeBindingRealization::kImageFailed;
  }
  return sampler ? NativeBindingRealization::kReady : NativeBindingRealization::kSamplerFailed;
}

constexpr bool NativeBindingFailed(NativeBindingRealization status) {
  return status == NativeBindingRealization::kSnapshotMissing ||
         status == NativeBindingRealization::kImageFailed ||
         status == NativeBindingRealization::kSamplerFailed;
}

constexpr const char* NativeBindingRealizationName(NativeBindingRealization status) {
  switch (status) {
    case NativeBindingRealization::kUnused: return "unused";
    case NativeBindingRealization::kGuestNull: return "guest-null";
    case NativeBindingRealization::kSnapshotMissing: return "snapshot-missing";
    case NativeBindingRealization::kImageFailed: return "image-failed";
    case NativeBindingRealization::kSamplerFailed: return "sampler-failed";
    case NativeBindingRealization::kReady: return "ready";
  }
  return "invalid";
}

constexpr bool NativeMissingPixelShaderIsIntentional(uint32_t guest_handle,
                                                    bool depth_stencil_active,
                                                    bool color_writes) {
  return guest_handle == 0 && depth_stencil_active && !color_writes;
}

}  // namespace rex::graphics::gta4_native
