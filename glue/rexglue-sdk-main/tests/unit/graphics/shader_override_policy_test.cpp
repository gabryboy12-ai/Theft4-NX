#include <array>

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/shader_override_policy.h"

namespace rex::graphics::gta4_native {
namespace {

constexpr uint64_t kVertexHash = 0xDF64C22EC010C136ull;
constexpr uint64_t kPixelHash = 0x458818340E2283DEull;
constexpr uint32_t kSmoothSupportBits = 0x3F59999Au;
constexpr uint32_t kSupportedSamples = 1u | 2u | 4u;
static constexpr std::array kVertexCounterparts{kPixelHash};
static constexpr std::array kPixelCounterparts{kVertexHash};
constexpr ShaderOverrideCandidate kVertexCandidate{
    true, ShaderOverrideActivation::kPipelinePair, 1, kVertexHash, kSmoothSupportBits,
    kSupportedSamples, kVertexCounterparts};
constexpr ShaderOverrideCandidate kPixelCandidate{
    true, ShaderOverrideActivation::kPipelinePair, 1, kPixelHash, kSmoothSupportBits,
    kSupportedSamples, kPixelCounterparts};

}  // namespace

TEST_CASE("GTA IV shader override mode parser fails closed to pair mode",
          "[gta4-native][graphics][shader-overrides]") {
  CHECK(ParseShaderOverrideMode("stock") == ShaderOverrideMode::kStock);
  CHECK(ParseShaderOverrideMode("stage") == ShaderOverrideMode::kStage);
  CHECK(ParseShaderOverrideMode("pair") == ShaderOverrideMode::kPair);
  CHECK(ParseShaderOverrideMode("unexpected") == ShaderOverrideMode::kPair);
}

TEST_CASE("GTA IV pair-scoped overrides activate only as a reciprocal pair",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kPair, kVertexCandidate,
                                     kPixelCandidate, 2u);
  STATIC_CHECK(selected.vertex_override);
  STATIC_CHECK(selected.pixel_override);
  STATIC_CHECK(selected.pipeline_pair_id == 1);
  STATIC_CHECK(selected.variant_key == MakeShaderOverrideVariantKey(true, true, 1));

  constexpr ShaderOverrideCandidate missing_pixel{};
  constexpr ShaderOverrideSelection incomplete =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kPair, kVertexCandidate,
                                     missing_pixel, 2u);
  STATIC_CHECK_FALSE(incomplete.vertex_override);
  STATIC_CHECK_FALSE(incomplete.pixel_override);
  STATIC_CHECK(incomplete.variant_key == 0);
}

TEST_CASE("GTA IV pair-scoped overrides reject unsupported attachment sample counts",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kPair, kVertexCandidate,
                                     kPixelCandidate, 8u);
  STATIC_CHECK_FALSE(selected.vertex_override);
  STATIC_CHECK_FALSE(selected.pixel_override);
  STATIC_CHECK(selected.variant_key == 0);
}

TEST_CASE("GTA IV pair-scoped overrides reject geometry that cannot contain pixel support",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideCandidate undersized_vertex{
      true, ShaderOverrideActivation::kPipelinePair, 1, kVertexHash, 0x3F400000u,
      kSupportedSamples, kVertexCounterparts};
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kPair, undersized_vertex,
                                     kPixelCandidate, 4u);
  STATIC_CHECK_FALSE(selected.vertex_override);
  STATIC_CHECK_FALSE(selected.pixel_override);
}

TEST_CASE("GTA IV pair mode keeps independent stage overrides enabled",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideCandidate vertex{
      true, ShaderOverrideActivation::kStage, 0, 0x1111ull, 0, 0, {}};
  constexpr ShaderOverrideCandidate pixel{
      true, ShaderOverrideActivation::kStage, 0, 0x2222ull, 0, 0, {}};
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kPair, vertex, pixel, 1u);
  STATIC_CHECK(selected.vertex_override);
  STATIC_CHECK(selected.pixel_override);
  STATIC_CHECK(selected.pipeline_pair_id == 0);
  STATIC_CHECK(selected.variant_key == MakeShaderOverrideVariantKey(true, true, 0));
}

TEST_CASE("GTA IV stage mode reproduces legacy independent selection",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideCandidate vertex{
      true, ShaderOverrideActivation::kPipelinePair, 7, 0x1111ull, 0, 0, {}};
  constexpr ShaderOverrideCandidate pixel{};
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kStage, vertex, pixel, 8u);
  STATIC_CHECK(selected.vertex_override);
  STATIC_CHECK_FALSE(selected.pixel_override);
  STATIC_CHECK(selected.pipeline_pair_id == 0);
}

TEST_CASE("GTA IV stock mode disables every override candidate",
          "[gta4-native][graphics][shader-overrides]") {
  constexpr ShaderOverrideCandidate vertex{
      true, ShaderOverrideActivation::kStage, 0, 0x1111ull, 0, 0, {}};
  constexpr ShaderOverrideCandidate pixel{
      true, ShaderOverrideActivation::kPipelinePair, 9, 0x2222ull, 0, 0, {}};
  constexpr ShaderOverrideSelection selected =
      ResolveShaderOverrideSelection(ShaderOverrideMode::kStock, vertex, pixel, 1u);
  STATIC_CHECK_FALSE(selected.vertex_override);
  STATIC_CHECK_FALSE(selected.pixel_override);
  STATIC_CHECK(selected.variant_key == 0);
}

}  // namespace rex::graphics::gta4_native
