// Diagnostic methods only. Called by graphics_system.cpp; no native state repair.
void Gta4NativeGraphicsSystem::TracePhoneNativeCommand(std::string_view point,
                                                       const NativeCommand& command) {
  if (!command.phone_trace) return;
  const auto& c = *command.phone_trace;
  PhoneTraceLog(point, fmt::format("run={} event={} occurrence={} seq={} epoch={} type={} scope={} replay={} device={:08X} frame={} cmd={} phase={} fixed-captured={:016X} fixed-recorded={:016X}",
      c.run, c.event, c.occurrence, command.diagnostic_submit_sequence,
      command.diagnostic_producer_epoch, CommandTypeName(command.type), c.scope, c.replay,
      c.device, point == "gpu-bound" ? diagnostic_submitted_frame_ : c.guest_frame,
      point == "gpu-bound" ? diagnostic_command_index_ : SIZE_MAX, uint32_t(command.render_phase),
      command.captured_fixed_function_state_hash, command.recorded_fixed_function_state_hash));
  const bool draw = command.type == CommandType::kDrawPrimitive ||
                    command.type == CommandType::kDrawPrimitiveUp ||
                    command.type == CommandType::kDrawIndexedPrimitive;
  if (draw || command.type == CommandType::kClear) {
    const auto& f = command.fixed_function_state;
    PhoneTraceLog("native-raster", fmt::format(
        "run={} event={} at={} viewport={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
        "blend-words={:08X},{:08X},{:08X},{:08X} sample-mask={:08X} alpha-to-mask={:08X}",
        c.run, c.event, point, f.viewport_bits[0], f.viewport_bits[1], f.viewport_bits[2],
        f.viewport_bits[3], f.viewport_bits[4], f.viewport_bits[5], f.blend_controls[0],
        f.blend_controls[1], f.blend_controls[2], f.blend_controls[3], f.sample_mask, f.alpha_to_mask));
    PhoneTraceLog("native-state", fmt::format("run={} event={} at={} depth={},{},{} clamp={} color-mask={:08X} blend={},{},{},{} alpha-test={},{},{} cull={} stencil={},{},{},{},{},{},{},{} back={},{},{},{},{},{},{} scissor={},{},{},{},{} constants={:016X},{:016X}",
        c.run, c.event, point, f.depth_enable, f.depth_function, f.depth_write_enable,
        f.depth_clamp_enable, f.color_write_mask, f.blend_enable, f.source_blend,
        f.destination_blend, f.blend_operation, f.alpha_test_enable, f.alpha_function,
        f.alpha_reference, f.cull_mode, f.stencil_enable, f.two_sided_stencil,
        f.stencil_function, f.stencil_fail, f.stencil_depth_fail, f.stencil_pass,
        f.stencil_reference, f.stencil_mask, f.ccw_stencil_function, f.ccw_stencil_fail,
        f.ccw_stencil_depth_fail, f.ccw_stencil_pass, f.back_stencil_reference,
        f.back_stencil_mask, f.back_stencil_write_mask, f.scissor_enable,
        f.scissor[0], f.scissor[1], f.scissor[2], f.scissor[3],
        command.vertex_constants_hash, command.pixel_constants_hash));
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      const auto& surface = command.pipeline_state ? command.pipeline_state->render_targets[i]
                                                   : command.snapshot_render_targets[i];
      PhoneTraceLog("native-attachment", fmt::format("run={} event={} at={} slot={} handle={:08X} address={:08X} size={}x{} format={} guest-samples={}",
          c.run, c.event, point, i, surface.handle, surface.address, surface.width,
          surface.height, surface.format, surface.sample_type));
    }
    const auto& depth = command.pipeline_state ? command.pipeline_state->depth_stencil
                                              : command.snapshot_depth_stencil;
    PhoneTraceLog("native-depth", fmt::format("run={} event={} at={} handle={:08X} address={:08X} size={}x{} format={} guest-samples={} stencil-write-mask={:08X}",
        c.run, c.event, point, depth.handle, depth.address, depth.width, depth.height,
        depth.format, depth.sample_type, f.stencil_write_mask));
  }
  if (command.type == CommandType::kClear) {
    ClearCommand clear{};
    std::memcpy(&clear, command.bytes.data(), sizeof(clear));
    PhoneTraceLog("native-clear", fmt::format("run={} event={} at={} flags={:08X} rect={},{},{},{} rgba={:08X},{:08X},{:08X},{:08X} depth={:016X} stencil={}",
        c.run, c.event, point, clear.flags, clear.left, clear.top, clear.right, clear.bottom,
        clear.color_bits[0], clear.color_bits[1], clear.color_bits[2], clear.color_bits[3], clear.depth_bits, clear.stencil));
  }
  if (command.type == CommandType::kResolve) {
    ResolveCommand resolve{};
    std::memcpy(&resolve, command.bytes.data(), sizeof(resolve));
    PhoneTraceLog("native-resolve", fmt::format("run={} event={} at={} flags={:08X} source={:08X}/{:08X} destination={:08X} generation={} level={} slice={} rect-valid={} rect={},{},{},{}",
        c.run, c.event, point, resolve.flags, resolve.source.handle, resolve.source.address,
        resolve.destination_texture, command.resolve_destination ? command.resolve_destination->generation : 0,
        resolve.destination_level, resolve.destination_slice_or_face, resolve.source_rectangle_valid,
        resolve.source_rectangle.left, resolve.source_rectangle.top, resolve.source_rectangle.right,
        resolve.source_rectangle.bottom));
  }
  if (command.type == CommandType::kTextureLock) {
    TextureLockCommand lock{};
    std::memcpy(&lock, command.bytes.data(), sizeof(lock));
    PhoneTraceLog("native-lock", fmt::format("run={} event={} at={} texture={:08X} level={} array={} flags={:08X}",
        c.run, c.event, point, lock.texture, lock.level, lock.array_index, lock.flags));
  }
  if (point == "gpu-bound" && command.pipeline_state) {
    const auto* vs = command.pipeline_state->vertex_shader_resource;
    const auto* ps = command.pipeline_state->pixel_shader_resource;
    PhoneTraceLog("native-shaders", fmt::format("run={} event={} vs={:016X} ps={:016X} used-textures={:08X}",
        c.run, c.event, vs ? vs->hash : 0, ps ? ps->hash : 0, command.used_texture_mask));
    for (uint32_t i = 0; i < kTextureStageCount; ++i) {
      if (!(command.used_texture_mask & (uint32_t(1) << i))) continue;
      const auto& texture = command.textures[i];
      const auto found = texture ? native_texture_images_.find(texture->generation)
                                : native_texture_images_.end();
      const auto* image = found != native_texture_images_.end() ? found->second.get() : nullptr;
      PhoneTraceLog("native-texture", fmt::format("run={} event={} stage={} handle={:08X} generation={} hash={:016X} image={} format={} layout={} size={}x{} resolved={} resolve-frame={} source-serial={} descriptor={} sampler={}",
          c.run, c.event, i, texture ? texture->handle : 0, texture ? texture->generation : 0,
          texture ? texture->content_hash : 0, image ? NativeVulkanHandleIdentity(image->resource.image) : 0,
          image ? uint32_t(image->format) : 0, image ? uint32_t(image->layout) : 0,
          image ? image->width : 0, image ? image->height : 0,
          image ? image->content.resolved : false, image ? image->content.last_resolved_frame : 0,
          image ? image->content.last_source_write_serial : 0,
          command.texture_descriptor_indices[i], command.sampler_descriptor_indices[i]));
    }
  }
}

static uint32_t PhoneProbeTexelBytes(VkFormat format, VkImageAspectFlags aspect) {
  if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return 1;
  if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
    switch (format) {
      case VK_FORMAT_D16_UNORM: return 2;
      case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
      case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return 4;
      default: return 0;
    }
  }
  switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
  }
}

void Gta4NativeGraphicsSystem::RecordPhoneProbe(VkCommandBuffer command_buffer,
    const NativeCommand& command, const NativeRenderingTarget& target,
    uint32_t frame, uint32_t command_index, uint32_t point) {
  if (!command.phone_trace || !PhoneTraceConfig().readbacks) return;
  const auto& c = *command.phone_trace;
  auto unavailable = [&](uint32_t attachment, VkImageAspectFlags aspect, std::string_view reason) {
    PhoneTraceLog("probe-unavailable", fmt::format("run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} reason={}",
        c.run, c.event, frame, command_index, point, attachment, uint32_t(aspect), reason));
  };
  if (!InitializeContentProbeBuffer(&phone_content_probe_buffer_)) {
    unavailable(UINT32_MAX, 0, "native-probes-disabled-or-buffer-allocation");
    return;
  }
  auto& buffer = phone_content_probe_buffer_;
  if (buffer.pending_frame && buffer.pending_frame != frame) {
    unavailable(UINT32_MAX, 0, "prior-frame-readback-pending");
    return;
  }
  if (!buffer.pending_frame) {
    std::memset(buffer.mapping, 0, size_t(kContentProbeBufferSize));
    buffer.pending_frame = frame;
    buffer.stages = {};
  }
  auto observe_bound = [&](NativeSurfaceImage* active, const SurfaceDescriptor& descriptor,
                           VkImageAspectFlags aspect, uint32_t attachment) -> NativeSurfaceImage* {
    if (active || !descriptor.handle) return active;
    // Do not allocate/prepare an inactive attachment to observe it. A reused
    // handle with multiple extant image versions is explicitly ambiguous.
    NativeSurfaceImage* found = nullptr;
    for (const auto& candidate : native_surface_images_) {
      if (pending_surface_release_ids_.contains(candidate->lifetime_id)) continue;
      const auto& d = candidate->descriptor;
      if (d.handle != descriptor.handle || d.address != descriptor.address ||
          d.width != descriptor.width || d.height != descriptor.height ||
          d.format != descriptor.format || d.sample_type != descriptor.sample_type ||
          !(candidate->aspect & aspect)) continue;
      if (found) {
        unavailable(attachment, aspect, "inactive-bound-image-ambiguous");
        return nullptr;
      }
      found = candidate.get();
    }
    return found;
  };
  auto record = [&](NativeSurfaceImage* image, uint32_t attachment, VkImageAspectFlags aspect, bool active) {
    if (!image) { unavailable(attachment, aspect, "bound-image-unavailable"); return; }
    const auto& stamp = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? image->aspect_content.color :
                        aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? image->aspect_content.depth : image->aspect_content.stencil;
    PhoneTraceLog("probe-image", fmt::format("run={} event={} frame={} cmd={} checkpoint={} attachment={} active={} aspect={} handle={:08X} address={:08X} image={} lifetime={} format={} samples={} size={}x{} layout={} initialized={} writer={}",
        c.run, c.event, frame, command_index, point, attachment, active, uint32_t(aspect),
        image->descriptor.handle, image->descriptor.address, NativeVulkanHandleIdentity(image->resource.image),
        image->lifetime_id, uint32_t(image->format), uint32_t(image->samples), image->width,
        image->height, uint32_t(image->layout), stamp.initialized, stamp.writer_serial));
    if (!(image->aspect & aspect)) { unavailable(attachment, aspect, "aspect-absent"); return; }
    // VUID-vkCmdCopyImageToBuffer-srcImage-07973: no implicit MSAA averaging.
    if (image->samples != VK_SAMPLE_COUNT_1_BIT) { unavailable(attachment, aspect, "multisampled-image"); return; }
    const uint32_t bytes_per_texel = PhoneProbeTexelBytes(image->format, aspect);
    if (!bytes_per_texel) { unavailable(attachment, aspect, "unsupported-format"); return; }
    if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED || !stamp.initialized) {
      unavailable(attachment, aspect, "undefined-content-or-layout"); return;
    }
    auto free_stage = std::find_if(buffer.stages.begin(), buffer.stages.end(),
        [](const auto& stage) { return !stage.valid && !stage.reserved; });
    if (free_stage == buffer.stages.end()) { unavailable(attachment, aspect, "probe-stage-budget"); return; }
    const uint32_t index = uint32_t(std::distance(buffer.stages.begin(), free_stage));
    auto identity = std::make_shared<PhoneProbeIdentity>();
    identity->context = c;
    identity->submit_sequence = command.diagnostic_submit_sequence;
    identity->epoch = command.diagnostic_producer_epoch;
    identity->frame = frame;
    identity->command_index = command_index;
    identity->point = point;
    identity->attachment = attachment;
    identity->image = NativeVulkanHandleIdentity(image->resource.image);
    identity->lifetime = image->lifetime_id;
    identity->writer_serial = stamp.writer_serial;
    identity->bytes_per_texel = bytes_per_texel;
    identity->ancestor_serial = stamp.ancestor_serial;
    identity->role = "bound-attachment";
    MarkNativeSurfaceImageUsed(*image);
    if (!RecordContentProbeImage(command_buffer, index, image->resource.image, image->format,
        image->layout, image->width, image->height, image->descriptor.handle,
        image->descriptor.address, 91, 0, aspect, 0, 1, &buffer)) {
      unavailable(attachment, aspect, "copy-recording-failed"); return;
    }
    free_stage->phone = std::move(identity);
    PhoneTraceLog("probe-scheduled", fmt::format("run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} stage={} image={} lifetime={} samples={} sample-stride={} grid={}x{}",
        c.run, c.event, frame, command_index, point, attachment, uint32_t(aspect), index,
        free_stage->phone->image, free_stage->phone->lifetime, kContentProbeSampleCount,
        kContentProbeSampleStride, kContentProbeAxis, kContentProbeAxis));
  };
  if (!command.pipeline_state) { unavailable(UINT32_MAX, 0, "pipeline-state-unavailable"); return; }
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!command.pipeline_state->render_targets[i].handle && !target.color_surfaces[i]) continue;
    record(observe_bound(target.color_surfaces[i], command.pipeline_state->render_targets[i],
                         VK_IMAGE_ASPECT_COLOR_BIT, i), i, VK_IMAGE_ASPECT_COLOR_BIT,
           target.color_surfaces[i] != nullptr);
  }
  if (command.pipeline_state->depth_stencil.handle || target.depth_surface) {
    auto* depth = observe_bound(target.depth_surface, command.pipeline_state->depth_stencil,
                                VK_IMAGE_ASPECT_DEPTH_BIT, kRenderTargetCount);
    record(depth, kRenderTargetCount, VK_IMAGE_ASPECT_DEPTH_BIT, target.depth_surface != nullptr);
    record(depth, kRenderTargetCount, VK_IMAGE_ASPECT_STENCIL_BIT, target.depth_surface != nullptr);
  }
}

void Gta4NativeGraphicsSystem::PublishPhoneProbe(const NativeContentProbeStage& stage,
                                                const uint8_t* bytes, bool visible) {
  if (!stage.phone) return;
  const auto& p = *stage.phone;
  const auto& c = p.context;
  auto failed = [&](std::string_view reason) {
    PhoneTraceLog("probe-unavailable", fmt::format("run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} reason={}",
        c.run, c.event, p.frame, p.command_index, p.point, p.attachment, uint32_t(stage.aspect), reason));
  };
  if (!visible || !p.submission || !bytes) { failed("gpu-completion-or-host-visibility-unproven"); return; }
  const auto directory = PhoneTraceOutputDirectory(c.run);
  if (directory.empty()) { failed("output-directory-unavailable"); return; }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) { failed("output-directory-create-failed"); return; }
  const auto stem = fmt::format("f{}-e{}-c{}-a{}-p{}", p.frame, c.event, p.point, p.attachment, uint32_t(stage.aspect));
  const auto binary_path = directory / (stem + ".bin");
  const auto metadata_path = directory / (stem + ".json");
  // Compact only actual texel bytes, excluding the staging buffer's padding.
  std::vector<uint8_t> compact(size_t(kContentProbeSampleCount) * p.bytes_per_texel);
  for (uint32_t sample = 0; sample < kContentProbeSampleCount; ++sample)
    std::memcpy(compact.data() + size_t(sample) * p.bytes_per_texel,
                bytes + size_t(sample) * kContentProbeSampleStride, p.bytes_per_texel);
  std::ofstream binary(binary_path, std::ios::binary | std::ios::trunc);
  binary.write(reinterpret_cast<const char*>(compact.data()), std::streamsize(compact.size()));
  binary.close();
  if (!binary) { failed("binary-write-failed"); return; }
  std::ofstream metadata(metadata_path, std::ios::trunc);
  metadata << "{\"schema\":1,\"run\":" << c.run << ",\"event\":" << c.event
           << ",\"occurrence\":" << c.occurrence << ",\"scope\":" << c.scope
           << ",\"seq\":" << p.submit_sequence << ",\"epoch\":" << p.epoch
           << ",\"frame\":" << p.frame << ",\"command_index\":" << p.command_index
           << ",\"checkpoint\":" << p.point << ",\"attachment\":" << p.attachment
           << ",\"aspect\":" << uint32_t(stage.aspect) << ",\"format\":" << uint32_t(stage.format)
           << ",\"width\":" << stage.width << ",\"height\":" << stage.height
           << ",\"handle\":" << stage.handle << ",\"address\":" << stage.address
           << ",\"image\":" << p.image << ",\"lifetime\":" << p.lifetime
           << ",\"writer_serial\":" << p.writer_serial << ",\"submission\":" << p.submission
           << ",\"ancestor_serial\":" << p.ancestor_serial
           << ",\"resource_generation\":" << p.resource_generation
           << ",\"resolve_frame\":" << p.resolve_frame << ",\"role\":\"" << p.role << "\""
           << ",\"fence_completed\":true,\"sparse_grid\":true,\"grid_axis\":" << kContentProbeAxis
           << ",\"samples\":" << kContentProbeSampleCount << ",\"bytes_per_texel\":" << p.bytes_per_texel
           << ",\"binary\":\"" << stem << ".bin\",\"bytes\":" << compact.size()
           << ",\"xxh3\":\"" << fmt::format("{:016X}", XXH3_64bits(compact.data(), compact.size())) << "\"}\n";
  metadata.close();
  if (!metadata) { failed("metadata-write-failed"); return; }
  PhoneTraceLog("probe-complete", fmt::format("run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} submission={} bytes={} metadata={}",
      c.run, c.event, p.frame, p.command_index, p.point, p.attachment, uint32_t(stage.aspect),
      p.submission, compact.size(), metadata_path.string()));
}


void Gta4NativeGraphicsSystem::AnalyzePendingPhoneProbe(uint32_t slot, uint64_t submission) {
  NativeContentProbeBuffer& buffer =
      slot == active_frame_slot_ ? phone_content_probe_buffer_ : secondary_phone_content_probe_buffer_;
  if (!buffer.pending_frame) return;

  auto* vulkan_provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const ui::vulkan::VulkanDevice* vulkan_device =
      vulkan_provider ? vulkan_provider->vulkan_device() : nullptr;
  bool memory_visible = vulkan_device && buffer.mapping && submission;
  for (const auto& stage : buffer.stages) {
    if (stage.phone && (!stage.phone->submission || stage.phone->submission > submission))
      memory_visible = false;
  }
  if (memory_visible && !(vulkan_device->memory_types().host_coherent &
                          (uint32_t(1) << buffer.memory_type))) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory;
    range.size = std::min(
        rex::round_up(kContentProbeBufferSize, vulkan_device->properties().nonCoherentAtomSize),
        buffer.allocation_size);
    memory_visible = vulkan_device->functions().vkInvalidateMappedMemoryRanges(
                         vulkan_device->device(), 1, &range) == VK_SUCCESS;
  }

  auto analysis_buffer = std::make_shared<NativeContentProbeBuffer>();
  auto analysis_payload = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kContentProbeBufferSize), uint8_t{});
  analysis_buffer->pending_frame = buffer.pending_frame;
  analysis_buffer->stages = std::move(buffer.stages);
  if (memory_visible) {
    std::memcpy(analysis_payload->data(), buffer.mapping, analysis_payload->size());
  }
  for (const auto& stage : analysis_buffer->stages) {
    if (stage.phone) {
      PhoneTraceLog("gpu-slot-complete", fmt::format(
          "run={} frame={} completed-submission={} slot={} host-visible={}",
          stage.phone->context.run, buffer.pending_frame, submission, slot, memory_visible));
      break;
    }
  }
  buffer.pending_frame = 0;
  buffer.stages = {};

  const auto analyze = [analysis_buffer, analysis_payload, memory_visible]() {
    for (uint32_t stage_index = 0; stage_index < analysis_buffer->stages.size(); ++stage_index) {
      const NativeContentProbeStage& stage = analysis_buffer->stages[stage_index];
      if (!stage.valid || stage.reserved || !stage.phone) continue;
      const uint8_t* bytes = memory_visible
                                 ? analysis_payload->data() +
                                       size_t(stage_index) * kContentProbeStageStride
                                 : nullptr;
      PublishPhoneProbe(stage, bytes, memory_visible);
    }
  };
  if (!EnqueueDeferredDiagnosticTask(analyze)) {
    RunLightProbeAnalysisInline(analyze);
  }
}

#include "phone_trace_lineage.inc"
