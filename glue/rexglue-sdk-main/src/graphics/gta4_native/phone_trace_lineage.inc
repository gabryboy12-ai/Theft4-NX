// Read-only GPU observations. Each copy restores the original image layout.
// Vulkan copy requirements: docs.vulkan.org/spec/latest/chapters/copies.html
// Source/resource lookup never creates or resolves a texture for diagnostics.
void Gta4NativeGraphicsSystem::RecordPhoneLineageImage(VkCommandBuffer cb,
    const NativeCommand& command, uint32_t frame, uint32_t cmd, uint32_t point,
    uint32_t attachment, NativeSurfaceImage* surface, NativeTextureImage* texture,
    std::string_view role) {
  if (!command.phone_trace || !PhoneTraceConfig().readbacks || !PhoneTraceConfig().lineage) return;
  const auto& c = *command.phone_trace;
  const auto unavailable = [&](VkImageAspectFlags aspect, std::string_view reason) {
    PhoneTraceLog("probe-unavailable", fmt::format(
        "run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} role={} reason={}",
        c.run, c.event, frame, cmd, point, attachment, aspect, role, reason));
  };
  if ((!surface && !texture) || (surface && texture)) { unavailable(0, "lineage-image-unavailable"); return; }
  const VkImage image = surface ? surface->resource.image : texture->resource.image;
  const VkFormat format = surface ? surface->format : texture->format;
  const VkImageLayout layout = surface ? surface->layout : texture->layout;
  const auto samples = surface ? surface->samples : texture->samples;
  const auto aspects = surface ? surface->aspect : texture->aspect;
  const auto width = surface ? surface->width : texture->width;
  const auto height = surface ? surface->height : texture->height;
  const auto lifetime = surface ? surface->lifetime_id : texture->descriptor_lifetime;
  const auto handle = surface ? surface->descriptor.handle : texture->source ? texture->source->handle : 0;
  const auto address = surface ? surface->descriptor.address : texture->source ? texture->source->info.memory.base_address : 0;
  const auto& content = surface ? surface->aspect_content : texture->aspect_content;
  if (texture && !(texture->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    unavailable(aspects, "lineage-texture-missing-transfer-src-usage"); return;
  }
  if (!InitializeContentProbeBuffer(&phone_content_probe_buffer_)) { unavailable(0,"lineage-buffer-unavailable"); return; }
  auto& buffer = phone_content_probe_buffer_;
  if (buffer.pending_frame && buffer.pending_frame != frame) { unavailable(0,"prior-frame-readback-pending"); return; }
  if (!buffer.pending_frame) {
    std::memset(buffer.mapping, 0, size_t(kContentProbeBufferSize));
    buffer.pending_frame = frame;
    buffer.stages = {};
  }
  for (const auto aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
    if (!(aspects & aspect)) continue;
    const auto& stamp = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? content.color :
                        aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? content.depth : content.stencil;
    PhoneTraceLog("lineage-image", fmt::format(
        "run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} role={} "
        "handle={:08X} image={} lifetime={} format={} samples={} size={}x{} layout={} "
        "initialized={} writer={} ancestor={} generation={} resolve-frame={}",
        c.run,c.event,frame,cmd,point,attachment,uint32_t(aspect),role,handle,
        NativeVulkanHandleIdentity(image),lifetime,uint32_t(format),uint32_t(samples),width,height,
        uint32_t(layout),stamp.initialized,stamp.writer_serial,stamp.ancestor_serial,
        texture && texture->source ? texture->source->generation : 0,
        texture ? texture->content.last_resolved_frame : 0));
    if (samples != VK_SAMPLE_COUNT_1_BIT) { unavailable(aspect,"multisampled-image"); continue; }
    const auto bpt = PhoneProbeTexelBytes(format,aspect);
    if (!bpt) { unavailable(aspect,"unsupported-format"); continue; }
    if (!image || !width || !height || layout == VK_IMAGE_LAYOUT_UNDEFINED || !stamp.initialized) {
      unavailable(aspect,"undefined-content-or-layout"); continue;
    }
    auto free = std::find_if(buffer.stages.begin(),buffer.stages.end(),[](const auto& s){return !s.valid && !s.reserved;});
    if (free==buffer.stages.end()) { unavailable(aspect,"probe-stage-budget"); continue; }
    const auto index=uint32_t(std::distance(buffer.stages.begin(),free));
    auto identity=std::make_shared<PhoneProbeIdentity>();
    identity->context=c; identity->submit_sequence=command.diagnostic_submit_sequence;
    identity->epoch=command.diagnostic_producer_epoch; identity->frame=frame;
    identity->command_index=cmd; identity->point=point; identity->attachment=attachment;
    identity->image=NativeVulkanHandleIdentity(image); identity->lifetime=lifetime;
    identity->writer_serial=stamp.writer_serial; identity->ancestor_serial=stamp.ancestor_serial;
    identity->bytes_per_texel=bpt; identity->role=role;
    identity->resource_generation=texture && texture->source ? texture->source->generation : 0;
    identity->resolve_frame=texture ? texture->content.last_resolved_frame : 0;
    if (surface) MarkNativeSurfaceImageUsed(*surface);
    else {
      texture->last_used_frame=std::max(texture->last_used_frame,active_texture_frame_);
      texture->last_used_submission=std::max(texture->last_used_submission,
          submission_tracker_ ? submission_tracker_->GetCurrentSubmission() : 0);
      StageNativeTextureFlightResources(*texture);
    }
    if (!RecordContentProbeImage(cb,index,image,format,layout,width,height,handle,address,91,0,aspect,0,1,&buffer)) {
      unavailable(aspect,"copy-recording-failed"); continue;
    }
    free->phone=std::move(identity);
    PhoneTraceLog("probe-scheduled",fmt::format(
        "run={} event={} frame={} cmd={} checkpoint={} attachment={} aspect={} stage={} image={} lifetime={} role={}",
        c.run,c.event,frame,cmd,point,attachment,uint32_t(aspect),index,
        free->phone->image,lifetime,role));
  }
}

void Gta4NativeGraphicsSystem::RecordPhoneHandoffProbe(VkCommandBuffer cb,
    const NativeCommand& command, uint32_t frame, uint32_t cmd, uint32_t point) {
  DepthSurfaceHandoffCommand handoff{};
  std::memcpy(&handoff,command.bytes.data(),sizeof(handoff));
  // No GetOrCreate call here: select an already-existing, descriptor-exact image.
  NativeSurfaceImage* destination=nullptr;
  for (const auto& candidate:native_surface_images_) {
    if (pending_surface_release_ids_.contains(candidate->lifetime_id)) continue;
    if (!SurfaceDescriptorsEqual(candidate->descriptor,handoff.destination)) continue;
    if (destination) {
      PhoneTraceLog("probe-unavailable",fmt::format(
          "run={} event={} frame={} cmd={} reason=handoff-destination-ambiguous",
          command.phone_trace->run,command.phone_trace->event,frame,cmd));
      destination=nullptr;break;
    }
    destination=candidate.get();
  }
  RecordPhoneLineageImage(cb,command,frame,cmd,point,40,destination,nullptr,"forward-handoff-destination");
  NativeTextureImage* source=nullptr;
  if (command.depth_handoff_source) {
    const auto found=native_texture_images_.find(command.depth_handoff_source->generation);
    if (found!=native_texture_images_.end()) source=found->second.get();
  }
  RecordPhoneLineageImage(cb,command,frame,cmd,point,41,nullptr,source,"resolved-gbuffer-depth-source");
}

void Gta4NativeGraphicsSystem::RecordPhoneLineageInputs(VkCommandBuffer cb,
    const NativeCommand& command, uint32_t frame, uint32_t cmd) {
  const auto& c=*command.phone_trace;
  for (uint32_t stage=0;stage<command.textures.size();++stage) {
    if (!(command.used_texture_mask & (uint32_t{1}<<stage))) continue;
    // Inspect the actual descriptor realization, not merely the guest handle.
    const NativeRoomLightInputBinding* binding=nullptr;
    if (command.room_light_input_bindings) {
      for (const auto& b:*command.room_light_input_bindings)
        if (b.texture_stage==stage) { binding=&b;break; }
    }
    NativeTextureImage* image=nullptr;
    if (binding) {
      for (const auto& [generation,candidate]:native_texture_images_)
        if (candidate->resource.image==binding->image && candidate->descriptor_lifetime==binding->image_lifetime) {
          image=candidate.get();break;
        }
    }
    PhoneTraceLog("consumed-binding",fmt::format(
        "run={} event={} frame={} cmd={} stage={} guest={:08X} realized={} image={} view={} "
        "lifetime={} sampler={} format={} extent={}x{} layout={} min-filter={} mag-filter={} "
        "address-u={} address-v={} mip-mode={} min-lod={} max-lod={} bias={} comparison={} op={} ",
        c.run,c.event,frame,cmd,stage,command.textures[stage]?command.textures[stage]->handle:0,
        binding!=nullptr,binding?NativeVulkanHandleIdentity(binding->image):0,
        binding?NativeVulkanHandleIdentity(binding->view):0,binding?binding->image_lifetime:0,
        binding?NativeVulkanHandleIdentity(binding->sampler):0,image?uint32_t(image->format):0,
        image?image->width:0,image?image->height:0,image?uint32_t(image->layout):0,
        binding?uint32_t(binding->sampler_info.minFilter):0,binding?uint32_t(binding->sampler_info.magFilter):0,
        binding?uint32_t(binding->sampler_info.addressModeU):0,binding?uint32_t(binding->sampler_info.addressModeV):0,
        binding?uint32_t(binding->sampler_info.mipmapMode):0,binding?binding->sampler_info.minLod:0,
        binding?binding->sampler_info.maxLod:0,binding?binding->sampler_info.mipLodBias:0,
        binding?uint32_t(binding->sampler_info.compareEnable):0,binding?uint32_t(binding->sampler_info.compareOp):0));
    if (!image || !binding || binding->view != image->resource.view) {
      PhoneTraceLog("probe-unavailable",fmt::format(
          "run={} event={} frame={} cmd={} stage={} reason=consumed-image-view-unproven",
          c.run,c.event,frame,cmd,stage));continue;
    }
    if (image->view_type!=VK_IMAGE_VIEW_TYPE_2D || image->view_range.baseMipLevel!=0 ||
        image->view_range.baseArrayLayer!=0) {
      PhoneTraceLog("probe-unavailable",fmt::format(
          "run={} event={} frame={} cmd={} stage={} reason=consumed-image-view-not-base-2d",
          c.run,c.event,frame,cmd,stage));continue;
    }
    RecordPhoneLineageImage(cb,command,frame,cmd,12,8+stage,nullptr,image,"consumed-texture-base-mip");
  }
}
