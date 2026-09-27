#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

// A normal depth transfer must retain destination-owned stencil. The title's
// full scene-to-forward transition instead creates a new classification.
enum class ForwardStencilHandoffPolicy : uint32_t {
  kPreserve = 0,
  kRebuildSceneCoverage = 1,
};

// PS_depthCopy0 (deferred_lighting_ps32.bin) exports 0.5 in the packed
// stencil byte when all three packed depth bytes are zero, otherwise 1.0.
// A native D/S image represents that color-alias write explicitly. This is
// not the G-buffer material stencil, nor a phone-dependent blanket reset.
inline constexpr uint32_t kForwardEmptySceneStencil = 0x80;
inline constexpr uint32_t kForwardCoveredSceneStencil = 0xFF;

constexpr uint32_t ForwardStencilFromPackedSceneDepth(uint32_t packed_depth) {
  return packed_depth ? kForwardCoveredSceneStencil : kForwardEmptySceneStencil;
}

constexpr bool IsValidForwardStencilHandoffPolicy(ForwardStencilHandoffPolicy policy) {
  return policy == ForwardStencilHandoffPolicy::kPreserve ||
         policy == ForwardStencilHandoffPolicy::kRebuildSceneCoverage;
}

}  // namespace rex::graphics::gta4_native
