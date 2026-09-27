#ifndef REX_GRAPHICS_GTA4_NATIVE_SHADER_OVERRIDE_POLICY_H_
#define REX_GRAPHICS_GTA4_NATIVE_SHADER_OVERRIDE_POLICY_H_

#include <cstdint>
#include <span>
#include <string_view>

namespace rex::graphics::gta4_native {

enum class ShaderOverrideMode : uint8_t {
  kStock,
  kStage,
  kPair,
};

enum class ShaderOverrideActivation : uint8_t {
  kStage,
  kPipelinePair,
};

struct ShaderOverrideCandidate {
  bool present = false;
  ShaderOverrideActivation activation = ShaderOverrideActivation::kStage;
  uint32_t pipeline_pair_id = 0;
  uint64_t shader_hash = 0;
  uint32_t support_radius_bits = 0;
  uint32_t supported_sample_count_mask = 0;
  std::span<const uint64_t> counterpart_hashes;
};

struct ShaderOverrideSelection {
  bool vertex_override = false;
  bool pixel_override = false;
  uint32_t pipeline_pair_id = 0;
  uint64_t variant_key = 0;
};

constexpr ShaderOverrideMode ParseShaderOverrideMode(std::string_view value) {
  if (value == "stock") {
    return ShaderOverrideMode::kStock;
  }
  if (value == "stage") {
    return ShaderOverrideMode::kStage;
  }
  return ShaderOverrideMode::kPair;
}

constexpr const char* ShaderOverrideModeName(ShaderOverrideMode mode) {
  switch (mode) {
    case ShaderOverrideMode::kStock:
      return "stock";
    case ShaderOverrideMode::kStage:
      return "stage";
    case ShaderOverrideMode::kPair:
      return "pair";
  }
  return "pair";
}

constexpr bool ShaderOverridePermitsCounterpart(const ShaderOverrideCandidate& candidate,
                                                uint64_t counterpart_hash) {
  for (uint64_t allowed_hash : candidate.counterpart_hashes) {
    if (allowed_hash == counterpart_hash) {
      return true;
    }
  }
  return false;
}

constexpr bool ShaderOverrideSupportsSampleCount(const ShaderOverrideCandidate& candidate,
                                                 uint32_t rasterization_samples) {
  return candidate.supported_sample_count_mask == 0 ||
         (candidate.supported_sample_count_mask & rasterization_samples) != 0;
}

// Pair metadata is generated only from finite positive IEEE-754 values. For
// positive floats, the uint32 bit pattern is monotonic, so this comparison is
// exact and remains constexpr without host floating-point evaluation.
constexpr bool ShaderOverrideSupportContains(const ShaderOverrideCandidate& vertex,
                                              const ShaderOverrideCandidate& pixel) {
  return vertex.support_radius_bits != 0 && pixel.support_radius_bits != 0 &&
         vertex.support_radius_bits >= pixel.support_radius_bits;
}

constexpr uint64_t MakeShaderOverrideVariantKey(bool vertex_override, bool pixel_override,
                                                uint32_t pipeline_pair_id) {
  return uint64_t(vertex_override ? 1u : 0u) | uint64_t(pixel_override ? 2u : 0u) |
         (uint64_t(pipeline_pair_id) << 32u);
}

constexpr ShaderOverrideSelection ResolveShaderOverrideSelection(
    ShaderOverrideMode mode, const ShaderOverrideCandidate& vertex,
    const ShaderOverrideCandidate& pixel, uint32_t rasterization_samples) {
  ShaderOverrideSelection selection;
  if (mode == ShaderOverrideMode::kStock) {
    return selection;
  }

  if (mode == ShaderOverrideMode::kStage) {
    // This diagnostic mode intentionally reproduces legacy independent-stage
    // selection, including combinations that pair mode rejects.
    selection.vertex_override = vertex.present;
    selection.pixel_override = pixel.present;
    selection.pipeline_pair_id =
        vertex.present && pixel.present && vertex.pipeline_pair_id == pixel.pipeline_pair_id
            ? vertex.pipeline_pair_id
            : 0;
    selection.variant_key = MakeShaderOverrideVariantKey(
        selection.vertex_override, selection.pixel_override, selection.pipeline_pair_id);
    return selection;
  }

  selection.vertex_override =
      vertex.present && vertex.activation == ShaderOverrideActivation::kStage &&
      ShaderOverrideSupportsSampleCount(vertex, rasterization_samples);
  selection.pixel_override =
      pixel.present && pixel.activation == ShaderOverrideActivation::kStage &&
      ShaderOverrideSupportsSampleCount(pixel, rasterization_samples);

  const bool coherent_pair =
      vertex.present && pixel.present &&
      vertex.activation == ShaderOverrideActivation::kPipelinePair &&
      pixel.activation == ShaderOverrideActivation::kPipelinePair && vertex.pipeline_pair_id != 0 &&
      vertex.pipeline_pair_id == pixel.pipeline_pair_id &&
      ShaderOverridePermitsCounterpart(vertex, pixel.shader_hash) &&
      ShaderOverridePermitsCounterpart(pixel, vertex.shader_hash) &&
      ShaderOverrideSupportsSampleCount(vertex, rasterization_samples) &&
      ShaderOverrideSupportsSampleCount(pixel, rasterization_samples) &&
      ShaderOverrideSupportContains(vertex, pixel);
  if (coherent_pair) {
    selection.vertex_override = true;
    selection.pixel_override = true;
    selection.pipeline_pair_id = vertex.pipeline_pair_id;
  }

  selection.variant_key = MakeShaderOverrideVariantKey(
      selection.vertex_override, selection.pixel_override, selection.pipeline_pair_id);
  return selection;
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_SHADER_OVERRIDE_POLICY_H_
