#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_CLIP_CONTROL_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_CLIP_CONTROL_H_

#include <cstdint>

namespace rex::graphics::gta4_native {

// Xenos PA_CL_CLIP_CNTL fields consumed by the native renderer.
constexpr uint32_t kNativeUserClipPlaneEnableMask = 0x3Fu;
constexpr uint32_t kNativeClipDisableMask = 1u << 16;
constexpr uint32_t kNativeUserClipCullOnlyMask = 1u << 17;
constexpr uint32_t kNativeDirectXClipSpaceMask = 1u << 19;

constexpr uint32_t NativeUserClipPlaneEnableMask(uint32_t clip_control) {
  return clip_control & kNativeUserClipPlaneEnableMask;
}

// Xenos clip-disable removes the hardware depth clip planes. Vulkan represents
// that behavior through depth clamp.
constexpr bool IsNativeDepthClampRequested(uint32_t clip_control) {
  return (clip_control & kNativeClipDisableMask) != 0;
}

// Vulkan's default clip interval is DirectX-style 0..W. The extension state is
// only needed when Xenos requests the negative-one-to-one interval.
constexpr bool IsNativeNegativeOneToOneClipSpace(uint32_t clip_control) {
  return (clip_control & kNativeDirectXClipSpaceMask) == 0;
}

constexpr bool IsNativeUserClipCullOnly(uint32_t clip_control) {
  return (clip_control & kNativeUserClipCullOnlyMask) != 0;
}

constexpr bool HasUnsupportedNativeUserClipPlanes(uint32_t clip_control) {
  const uint32_t enabled_planes = NativeUserClipPlaneEnableMask(clip_control);
  return (enabled_planes & ~1u) != 0 ||
         (enabled_planes != 0 && IsNativeUserClipCullOnly(clip_control));
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_CLIP_CONTROL_H_
