#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <rex/graphics/gta4_native/title_commands.h>

namespace rex::graphics::gta4_native {

struct NativeResolveWriteRegion {
  int32_t destination_x = 0;
  int32_t destination_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
};

// A native resolve destination has two independent facts to track. A
// deterministic initialization makes every texel legal to sample, while a
// completed title resolve establishes that the image contains title-produced
// content. Keeping those facts separate lets an unwritten resolve preserve a
// defined fallback without pretending that the title rendered it.
struct NativeResolvedTextureContentState {
  bool defined = false;
  bool resolved = false;
  uint32_t last_resolved_frame = 0;
  uint64_t last_source_write_serial = 0;

  constexpr void MarkDeterministicallyInitialized() { defined = true; }

  constexpr void CommitResolve(uint32_t frame, uint64_t source_write_serial) {
    defined = true;
    resolved = true;
    last_resolved_frame = frame;
    last_source_write_serial = source_write_serial;
  }
};

// GTA intentionally keeps reflection captures alive when a frame doesn't
// produce a replacement. Ordinary resolve destinations are frame-local
// intermediates: treating an unwritten ordinary resolve as successful would
// relabel a previous frame's image as current post-processing input.
constexpr bool CanPreserveNativeResolvedTexture(
    const NativeResolvedTextureContentState& content, bool reflection_resolve) {
  return reflection_resolve && content.defined;
}

// A resolve may discard the destination's previous contents only when the
// shader overwrites the complete destination subresource. Partial resolves
// must retain LOAD semantics because GTA can assemble a texture incrementally.
constexpr bool IsFullNativeResolveSubresourceOverwrite(const NativeResolveWriteRegion& region) {
  return region.target_width != 0 && region.target_height != 0 && region.destination_x == 0 &&
         region.destination_y == 0 && region.width == region.target_width &&
         region.height == region.target_height;
}

constexpr bool RequiresNativeResolveScaleConversion(
    uint32_t source_width, uint32_t source_height, uint32_t destination_width,
    uint32_t destination_height) {
  return source_width != destination_width || source_height != destination_height;
}

struct NativeColorResolveFusionInput {
  NativeResolveWriteRegion destination{};
  uint32_t mirror_width = 0;
  uint32_t mirror_height = 0;
  bool color_attachments_supported = false;
  bool mirror_is_single_level_fp16_color = false;
  bool distinct_images = false;
  bool level_zero = false;
};

constexpr bool CanFuseNativeColorResolve(const NativeColorResolveFusionInput& input) {
  return input.color_attachments_supported && input.mirror_is_single_level_fp16_color &&
         input.distinct_images && input.level_zero &&
         IsFullNativeResolveSubresourceOverwrite(input.destination) &&
         input.mirror_width == input.destination.target_width &&
         input.mirror_height == input.destination.target_height;
}

// Compare the immutable inputs to ResolveRenderingTarget, including inactive
// descriptors. Exact equality is deliberately stricter than the surface cache's
// placement equivalence, and cannot hide a color-target or write-mask scope break.
constexpr bool NativeProducerResolveDescriptorsEqual(const SurfaceDescriptor& left,
                                                     const SurfaceDescriptor& right) {
  return left.handle == right.handle && left.flags == right.flags && left.base == right.base &&
         left.address == right.address && left.packed_dimensions == right.packed_dimensions &&
         left.format == right.format && left.width == right.width && left.height == right.height &&
         left.sample_type == right.sample_type;
}

struct NativeProducerDepthResolveScope {
  std::array<SurfaceDescriptor, kRenderTargetCount> colors{};
  SurfaceDescriptor depth_stencil{};
  uint32_t color_attachment_mask = 0;
  uint32_t color_write_mask = 0;
  uint32_t depth_stencil_aspects = 0;
  RenderPhase phase = RenderPhase::kUnknown;
  uint32_t phase_object = 0;

  constexpr bool operator==(const NativeProducerDepthResolveScope& other) const {
    return std::equal(colors.begin(), colors.end(), other.colors.begin(),
                      NativeProducerResolveDescriptorsEqual) &&
           NativeProducerResolveDescriptorsEqual(depth_stencil, other.depth_stencil) &&
           color_attachment_mask == other.color_attachment_mask &&
           color_write_mask == other.color_write_mask &&
           depth_stencil_aspects == other.depth_stencil_aspects &&
           phase == other.phase && phase_object == other.phase_object;
  }
};

inline constexpr uint32_t kNativeProducerDepthResolveLookahead = 4096;

enum class NativeProducerDepthResolveScanStep { kCompatible, kBoundary, kResolve };

// A frame-local, constant-size cursor. A failed target interval is never scanned
// again; its boundary may be inspected once more as the next interval's start.
// Matching a resolve claims the entire interval even if subsequent preflight
// fails or a hidden scope break requires the normal logical-command fallback.
// Thus each title resolve can have at most one producer attachment attempt.
struct NativeProducerDepthResolveScan {
  size_t next_command_index = 0;

  template <typename Classify>
  size_t FindResolve(size_t command_index, size_t command_count, Classify classify) {
    if (command_index < next_command_index || command_index >= command_count) {
      return SIZE_MAX;
    }
    const size_t count = std::min<size_t>(command_count - command_index,
                                        kNativeProducerDepthResolveLookahead);
    for (size_t offset = 0; offset < count; ++offset) {
      const size_t index = command_index + offset;
      const NativeProducerDepthResolveScanStep step = classify(index);
      if (step == NativeProducerDepthResolveScanStep::kBoundary) {
        next_command_index = index == command_index ? index + 1 : index;
        return SIZE_MAX;
      }
      next_command_index = index + 1;
      if (step == NativeProducerDepthResolveScanStep::kResolve) {
        return index == command_index ? SIZE_MAX : index;
      }
    }
    return SIZE_MAX;
  }
};

// An early physical overwrite must already satisfy the logical command's
// geometry and clear contract. Unknown flags and clear-packing overrides stay
// on the established path; those overrides are not destination-format changes.
constexpr bool IsNativeProducerDepthResolveRequest(
    const ResolveCommand& resolve, uint32_t source_logical_width,
    uint32_t source_logical_height, uint32_t destination_logical_width,
    uint32_t destination_logical_height, bool exact_sample_space,
    bool clear_depth_targets_source) {
  constexpr uint32_t allowed_flags = 0x277u;  // source, sample selector, depth clear
  return (resolve.flags & 7u) == 4u && !(resolve.flags & ~allowed_flags) &&
         !resolve.parameters_valid && resolve.destination_level == 0 &&
         resolve.destination_slice_or_face == 0 && exact_sample_space &&
         (!(resolve.flags & 0x200u) || clear_depth_targets_source) &&
         source_logical_width && source_logical_height &&
         source_logical_width <= INT32_MAX && source_logical_height <= INT32_MAX &&
         resolve.source.width == source_logical_width &&
         resolve.source.height == source_logical_height &&
         destination_logical_width == source_logical_width &&
         destination_logical_height == source_logical_height &&
         (!resolve.source_rectangle_valid ||
          (resolve.source_rectangle.left == 0 && resolve.source_rectangle.top == 0 &&
           resolve.source_rectangle.right == int32_t(source_logical_width) &&
           resolve.source_rectangle.bottom == int32_t(source_logical_height))) &&
         (!resolve.destination_point_valid ||
          (resolve.destination_point.x == 0 && resolve.destination_point.y == 0));
}

// Attachment resolves cannot express offsets, scaling, partial writes, or a
// guest sample selector other than the device's supported sample-zero mode.
struct NativeProducerDepthResolveEligibility {
  NativeResolveWriteRegion destination{};
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  uint32_t render_width = 0;
  uint32_t render_height = 0;
  bool exact_source_view = false;
  bool full_source_rectangle = false;
  bool combined_depth_stencil = false;
  bool matching_formats = false;
  bool multisampled_source = false;
  bool sample_zero_requested = false;
  bool sample_zero_supported = false;
  bool single_subresource_destination = false;
  bool distinct_images = false;
  bool destination_unused_until_resolve = false;
};

constexpr bool CanAttachNativeProducerDepthResolve(
    const NativeProducerDepthResolveEligibility& input) {
  return input.exact_source_view && input.full_source_rectangle &&
         input.combined_depth_stencil && input.matching_formats && input.multisampled_source &&
         input.sample_zero_requested && input.sample_zero_supported &&
         input.single_subresource_destination && input.distinct_images &&
         input.destination_unused_until_resolve &&
         IsFullNativeResolveSubresourceOverwrite(input.destination) &&
         input.source_width == input.destination.target_width &&
         input.source_height == input.destination.target_height &&
         input.render_width == input.source_width && input.render_height == input.source_height;
}

// This is recording-local metadata, never a cache or a resource owner. Every
// identity is checked again at the title command, after any intervening scope
// break, clear, alias materialization, or draw. Publication remains at that
// command. Vulkan objects themselves use the normal submission retirement.
struct NativeProducerDepthResolveStamp {
  uint64_t command_index = UINT64_MAX;
  uint64_t submission = 0;
  uint64_t source_lifetime = 0;
  uint64_t source_image = 0;
  uint64_t destination_lifetime = 0;
  uint64_t destination_image = 0;
  uint64_t destination_generation = 0;
  uint64_t depth_serial = 0;
  uint64_t stencil_serial = 0;

  constexpr bool operator==(const NativeProducerDepthResolveStamp&) const = default;
};

constexpr bool CanPublishNativeProducerDepthResolve(
    const NativeProducerDepthResolveStamp& recorded,
    const NativeProducerDepthResolveStamp& current) {
  return recorded.command_index != UINT64_MAX && recorded.submission &&
         recorded.source_lifetime && recorded.source_image && recorded.destination_lifetime &&
         recorded.destination_image && recorded.destination_generation && recorded.depth_serial &&
         recorded.stencil_serial && recorded == current;
}

}  // namespace rex::graphics::gta4_native
