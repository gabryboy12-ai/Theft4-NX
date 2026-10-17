// Exact same-format D32/S8 transfer through integer scratch, kept separate
// from converted depth ownership shaders. All resources preflight before GPU
// commands; existing copy path remains the fallback.
void VulkanRenderTargetCache::DestroyRawDepthScratch(RawDepthScratch& scratch) {
  const auto* device = command_processor_.GetVulkanDevice();
  const auto& dfn = device->functions();
  const VkDevice handle = device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, handle, scratch.framebuffer.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, handle, scratch.descriptor_pool);
  for (uint32_t i = 0; i < 2; ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, handle, scratch.views[i]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, handle, scratch.images[i]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, handle, scratch.memory[i]);
  }
}

VulkanRenderTargetCache::RawDepthScratch* VulkanRenderTargetCache::GetRawDepthScratch(
    VkExtent2D extent, VkSampleCountFlagBits samples,
    const std::vector<RawDepthScratch*>& reserved) {
  constexpr VkDeviceSize budget = 64 * 1024 * 1024;
  if (!extent.width || !extent.height) return nullptr;
  for (auto& scratch : raw_depth_scratch_) {
    if (scratch->samples == samples && scratch->framebuffer.host_extent.width >= extent.width &&
        scratch->framebuffer.host_extent.height >= extent.height &&
        std::find(reserved.begin(), reserved.end(), scratch.get()) == reserved.end()) return scratch.get();
  }
  if (raw_depth_scratch_.size() >= 8 || raw_depth_scratch_bytes_ >= budget ||
      uint64_t(extent.width) * extent.height * uint32_t(samples) * 5 > budget - raw_depth_scratch_bytes_) return nullptr;
  const auto* device = command_processor_.GetVulkanDevice();
  const auto& dfn = device->functions();
  const VkDevice handle = device->device();
  if (extent.width > device->properties().maxFramebufferWidth ||
      extent.height > device->properties().maxFramebufferHeight) return nullptr;
  const uint32_t sample_index = samples == VK_SAMPLE_COUNT_1_BIT ? 0 : samples == VK_SAMPLE_COUNT_2_BIT ? 1 : 2;
  constexpr VkFormat formats[2] = {VK_FORMAT_R32_UINT, VK_FORMAT_R8_UINT};
  constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  for (VkFormat format : formats) {
    VkImageFormatProperties props{};
    if (device->vulkan_instance()->functions().vkGetPhysicalDeviceImageFormatProperties(
        device->physical_device(), format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        usage, 0, &props) != VK_SUCCESS || !(props.sampleCounts & samples) ||
        props.maxExtent.width < extent.width || props.maxExtent.height < extent.height) return nullptr;
  }
  if (!raw_depth_render_passes_[sample_index]) {
    VkAttachmentDescription attachments[2]{};
    VkAttachmentReference refs[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      attachments[i].format = formats[i]; attachments[i].samples = samples;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[i].initialLayout = attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2; subpass.pColorAttachments = refs;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2; info.pAttachments = attachments;
    info.subpassCount = 1; info.pSubpasses = &subpass;
    if (dfn.vkCreateRenderPass(handle, &info, nullptr, &raw_depth_render_passes_[sample_index]) != VK_SUCCESS) return nullptr;
  }
  auto scratch = std::make_unique<RawDepthScratch>();
  auto fail = [&]() -> RawDepthScratch* { DestroyRawDepthScratch(*scratch); return nullptr; };
  for (uint32_t i = 0; i < 2; ++i) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D; image.format = formats[i];
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = image.arrayLayers = 1; image.samples = samples;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkDeviceSize allocated = 0;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, image,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, scratch->images[i], scratch->memory[i],
        nullptr, &allocated)) return fail();
    if (allocated > budget - raw_depth_scratch_bytes_ - scratch->bytes) return fail();
    scratch->bytes += allocated;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = scratch->images[i]; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = formats[i];
    view.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    if (dfn.vkCreateImageView(handle, &view, nullptr, &scratch->views[i]) != VK_SUCCESS) return fail();
  }
  VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer.renderPass = raw_depth_render_passes_[sample_index];
  framebuffer.attachmentCount = 2; framebuffer.pAttachments = scratch->views;
  framebuffer.width = extent.width; framebuffer.height = extent.height; framebuffer.layers = 1;
  if (dfn.vkCreateFramebuffer(handle, &framebuffer, nullptr, &scratch->framebuffer.framebuffer) != VK_SUCCESS) return fail();
  scratch->framebuffer.host_extent = extent; scratch->samples = samples;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
  if (dfn.vkCreateDescriptorPool(handle, &pool, nullptr, &scratch->descriptor_pool) != VK_SUCCESS) return fail();
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = scratch->descriptor_pool; alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &descriptor_set_layout_sampled_image_x2_;
  if (dfn.vkAllocateDescriptorSets(handle, &alloc, &scratch->descriptor_set) != VK_SUCCESS) return fail();
  VkDescriptorImageInfo images[2]{}; VkWriteDescriptorSet writes[2]{};
  for (uint32_t i = 0; i < 2; ++i) {
    images[i].imageView = scratch->views[i]; images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = scratch->descriptor_set; writes[i].dstBinding = i;
    writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(handle, 2, writes, 0, nullptr);
  raw_depth_scratch_bytes_ += scratch->bytes;
  raw_depth_scratch_.push_back(std::move(scratch));
  return raw_depth_scratch_.back().get();
}

const VkPipeline* VulkanRenderTargetCache::GetRawDepthPipelines(uint32_t destination_pitch,
    uint32_t source_pitch, xenos::MsaaSamples guest_samples, bool dynamic) {
  const auto* device = command_processor_.GetVulkanDevice();
  const uint64_t key = (uint64_t(dynamic) << 63) | (uint64_t(destination_pitch) << 32) |
      (uint64_t(source_pitch) << 2) | uint32_t(guest_samples);
  auto found = raw_depth_pipelines_.find(key);
  if (found != raw_depth_pipelines_.end()) return found->second[0] ? found->second.data() : nullptr;
  const auto& dfn = device->functions(); const VkDevice handle = device->device();
  const uint32_t samples = guest_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_
      ? 4 : uint32_t(1) << uint32_t(guest_samples);
  const uint32_t sample_index = samples == 1 ? 0 : samples == 2 ? 1 : 2;
  if (!raw_depth_pipeline_layout_) {
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &descriptor_set_layout_sampled_image_x2_;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    if (dfn.vkCreatePipelineLayout(handle, &layout, nullptr, &raw_depth_pipeline_layout_) != VK_SUCCESS) return nullptr;
  }
  const bool sample_rate = device->properties().sampleRateShading;
  const uint32_t pipeline_count = samples > 1 && !sample_rate ? samples : 1;
  std::array<VkPipeline, 4> pipelines{};
  for (uint32_t sample = 0; sample < pipeline_count; ++sample) {
    auto code = raw_depth::GenerateFragment(destination_pitch, source_pitch,
        xenos::kEdramTileWidthSamples >> uint32_t(guest_samples >= xenos::MsaaSamples::k4X),
        xenos::kEdramTileHeightSamples >> uint32_t(guest_samples >= xenos::MsaaSamples::k2X),
        samples, sample_rate, sample);
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = code.size() * sizeof(uint32_t); shader.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (dfn.vkCreateShaderModule(handle, &shader, nullptr, &module) != VK_SUCCESS) break;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main";}
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = transfer_passthrough_vertex_shader_;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = module;
    VkVertexInputBindingDescription binding{0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding; vertex.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    VkSampleMask mask = pipeline_count > 1 ? uint32_t(1) << sample : UINT32_MAX;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VkSampleCountFlagBits(samples);
    multisample.sampleShadingEnable = samples > 1 && sample_rate; multisample.minSampleShading = 1;
    multisample.pSampleMask = &mask;
    VkPipelineColorBlendAttachmentState attachments[2]{};
    for (auto& attachment : attachments) attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 2; blend.pAttachments = attachments;
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    state.dynamicStateCount = 2; state.pDynamicStates = states;
    VkFormat formats[2] = {VK_FORMAT_R32_UINT, VK_FORMAT_R8_UINT};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 2; rendering.pColorAttachmentFormats = formats;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = dynamic ? &rendering : nullptr;
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly; info.pViewportState = &viewport;
    info.pRasterizationState = &raster; info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend; info.pDynamicState = &state;
    info.layout = raw_depth_pipeline_layout_;
    info.renderPass = dynamic ? VK_NULL_HANDLE : raw_depth_render_passes_[sample_index];
    VkResult result = dfn.vkCreateGraphicsPipelines(handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[sample]);
    dfn.vkDestroyShaderModule(handle, module, nullptr);
    if (result != VK_SUCCESS) break;
  }
  for (uint32_t i = 0; i < pipeline_count; ++i) {
    if (pipelines[i]) continue;
    for (auto pipeline : pipelines) if (pipeline) dfn.vkDestroyPipeline(handle, pipeline, nullptr);
    raw_depth_pipelines_.emplace(key, std::array<VkPipeline, 4>{}); return nullptr;
  }
  return raw_depth_pipelines_.emplace(key, pipelines).first->second.data();
}

bool VulkanRenderTargetCache::TryRawDepthOwnershipTransfers(VulkanRenderTarget& destination,
    const std::vector<Transfer>& transfers) {
  const auto key = destination.key();
  const auto* device = command_processor_.GetVulkanDevice();
  if (!REXCVAR_GET(vulkan_raw_depth_ownership) || !key.is_depth ||
      key.GetDepthFormat() != xenos::DepthRenderTargetFormat::kD24FS8 ||
      GetDepthVulkanFormat(key.GetDepthFormat()) != VK_FORMAT_D32_SFLOAT_S8_UINT ||
      !rex::ui::vulkan::StencilCopyEnabled(device->device()) || transfers.empty() || transfers.size() > 7 ||
      draw_resolution_scale_x() != 1 || draw_resolution_scale_y() != 1) return false;
  const bool dynamic = REXCVAR_GET(vulkan_dynamic_rendering) && device->properties().dynamicRendering;
  const uint32_t physical_samples = key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_
      ? 4 : uint32_t(1) << uint32_t(key.msaa_samples);
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> uint32_t(key.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X);
  const ownership_copy::Surface dst_surface{key.base_tiles, key.GetPitchTiles(), key.GetWidth(),
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples)};
  struct Plan {
    VulkanRenderTarget* source;
    std::vector<ownership_copy::Rectangle> rectangles;
    raw_depth::Bounds source_bounds;
    RawDepthScratch* mirror = nullptr;
    const VkPipeline* pipelines = nullptr;
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceSize vertices_offset = 0;
  };
  std::vector<Plan> plans;
  std::vector<std::vector<ownership_copy::Rectangle>> all_rectangles;
  raw_depth::Bounds dst_bounds;
  uint64_t pixels = 0, source_bbox_pixels = 0, rectangle_count = 0, old_regions = 0;
  for (const auto& transfer : transfers) {
    if (!transfer.source || transfer.source == &destination) return false;
    auto& source = *static_cast<VulkanRenderTarget*>(transfer.source);
    const auto source_key = source.key();
    if (!source.image_copy_supported() || source.image() == destination.image() || !source_key.is_depth ||
        source_key.resource_format != key.resource_format || source_key.msaa_samples != key.msaa_samples) return false;
    ownership_copy::Surface src_surface{source_key.base_tiles, source_key.GetPitchTiles(), source_key.GetWidth(),
        GetRenderTargetHeight(source_key.pitch_tiles_at_32bpp, source_key.msaa_samples)};
    Transfer::Rectangle spans[Transfer::kMaxRectanglesWithCutout];
    const auto count = transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(), key.msaa_samples, false, spans);
    Plan plan{&source};
    for (uint32_t i = 0; i < count; ++i) {
      const auto& s = spans[i]; ownership_copy::Rectangle rectangle{s.x_pixels, s.y_pixels, s.width_pixels, s.height_pixels};
      if (!raw_depth::UnwrappedTiles(src_surface, dst_surface, tile_width, tile_height, rectangle) ||
          !dst_bounds.Include(rectangle.x, rectangle.y, rectangle.width, rectangle.height)) return false;
      plan.rectangles.push_back(rectangle);
    }
    std::vector<ownership_copy::Region> regions;
    if (!ownership_copy::BuildRegions(src_surface, dst_surface, tile_width, tile_height,
        plan.rectangles.data(), uint32_t(plan.rectangles.size()), regions)) return false;
    for (const auto& region : regions) {
      if (!plan.source_bounds.Include(region.source_x, region.source_y, region.width, region.height)) return false;
      pixels += uint64_t(region.width) * region.height;
    }
    old_regions += regions.size(); rectangle_count += plan.rectangles.size();
    source_bbox_pixels += plan.source_bounds.pixels();
    all_rectangles.push_back(plan.rectangles); plans.push_back(std::move(plan));
  }
  if (old_regions > 8192 || !ownership_copy::PairwiseDisjoint(all_rectangles) ||
      !raw_depth::Worthwhile(old_regions, plans.size(), rectangle_count, pixels,
                            source_bbox_pixels, dst_bounds.pixels())) return false;
  std::vector<RawDepthScratch*> reserved;
  auto* output = GetRawDepthScratch({dst_bounds.width(), dst_bounds.height()},
      VkSampleCountFlagBits(physical_samples), reserved);
  if (!output) return false;
  reserved.push_back(output);
  const uint32_t viewport_width = rex::next_pow2(output->framebuffer.host_extent.width);
  const uint32_t viewport_height = rex::next_pow2(output->framebuffer.host_extent.height);
  if (viewport_width > device->properties().maxViewportDimensions[0] ||
      viewport_height > device->properties().maxViewportDimensions[1]) return false;
  const float pixels_to_ndc_x = 2.0f / float(viewport_width);
  const float pixels_to_ndc_y = 2.0f / float(viewport_height);
  for (auto& plan : plans) {
    plan.mirror = GetRawDepthScratch({plan.source_bounds.width(), plan.source_bounds.height()},
        VkSampleCountFlagBits(physical_samples), reserved);
    if (!plan.mirror) return false;
    reserved.push_back(plan.mirror);
    plan.pipelines = GetRawDepthPipelines(key.GetPitchTiles(), plan.source->key().GetPitchTiles(), key.msaa_samples, dynamic);
    if (!plan.pipelines) return false;
    const auto vertex_count = uint32_t(plan.rectangles.size()) * 6;
    float* data = reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
        command_processor_.GetCurrentSubmission(), sizeof(float) * 2 * vertex_count,
        sizeof(float), plan.vertices, plan.vertices_offset));
    if (!data) return false;
    for (const auto& rectangle : plan.rectangles) {
      const float x0 = -1 + (rectangle.x - dst_bounds.x) * pixels_to_ndc_x;
      const float y0 = -1 + (rectangle.y - dst_bounds.y) * pixels_to_ndc_y;
      const float x1 = -1 + (rectangle.x + rectangle.width - dst_bounds.x) * pixels_to_ndc_x;
      const float y1 = -1 + (rectangle.y + rectangle.height - dst_bounds.y) * pixels_to_ndc_y;
      for (float value : {x0,y0,x0,y1,x1,y0,x1,y0,x0,y1,x1,y1}) *(data++) = value;
    }
  }
  // All memory, descriptors, shader pipelines, and vertex uploads are ready.
  // No failure can redirect a partially emitted transfer back to the old path.
  auto& commands = command_processor_.deferred_command_buffer();
  auto transition = [&](RawDepthScratch& scratch, VkPipelineStageFlags stage, VkAccessFlags access, VkImageLayout layout) {
    for (VkImage image : scratch.images) command_processor_.PushImageMemoryBarrier(image,
        ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT), scratch.stage, stage,
        scratch.access, access, scratch.layout, layout);
    scratch.stage = stage; scratch.access = access; scratch.layout = layout;
  };
  command_processor_.EndRenderPass();
  for (auto& plan : plans) {
    auto& source = *plan.source;
    command_processor_.PushImageMemoryBarrier(source.image(),
        ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
        source.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT, source.current_access_mask(),
        VK_ACCESS_TRANSFER_READ_BIT, source.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    source.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*plan.mirror, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  command_processor_.SubmitBarriers(true);
  for (auto& plan : plans) for (uint32_t aspect = 0; aspect < 2; ++aspect) {
    VkImageCopy copy{};
    copy.srcSubresource = {aspect ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = {int32_t(plan.source_bounds.x), int32_t(plan.source_bounds.y), 0};
    copy.extent = {plan.source_bounds.width(), plan.source_bounds.height(), 1};
    commands.CmdVkCopyImage(plan.source->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        plan.mirror->images[aspect], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  }
  for (auto& plan : plans) transition(*plan.mirror, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  transition(*output, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const uint32_t sample_index = physical_samples == 1 ? 0 : physical_samples == 2 ? 1 : 2;
  command_processor_.SubmitBarriersAndEnterRawDepthRenderPass(raw_depth_render_passes_[sample_index],
      &output->framebuffer, output->views, {dst_bounds.width(), dst_bounds.height()}, dynamic);
  VkViewport viewport{0, 0, float(viewport_width), float(viewport_height), 0, 1};
  VkRect2D scissor{{0, 0}, {dst_bounds.width(), dst_bounds.height()}};
  command_processor_.SetViewport(viewport); command_processor_.SetScissor(scissor);
  commands.CmdSetRenderOperation(DeferredCommandBuffer::GpuOperation::kOwnershipDepth);
  const uint32_t pipeline_count = physical_samples > 1 && !device->properties().sampleRateShading ? physical_samples : 1;
  for (const auto& plan : plans) {
    uint32_t push[5] = {uint32_t(int32_t(key.base_tiles) - int32_t(plan.source->key().base_tiles)),
        dst_bounds.x, dst_bounds.y, plan.source_bounds.x, plan.source_bounds.y};
    commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, raw_depth_pipeline_layout_, 0,
        1, &plan.mirror->descriptor_set, 0, nullptr);
    commands.CmdVkPushConstants(raw_depth_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    commands.CmdVkBindVertexBuffers(0, 1, &plan.vertices, &plan.vertices_offset);
    for (uint32_t sample = 0; sample < pipeline_count; ++sample) {
      command_processor_.BindExternalGraphicsPipeline(plan.pipelines[sample]);
      commands.CmdVkDraw(uint32_t(plan.rectangles.size()) * 6, 1, 0, 0);
    }
  }
  commands.CmdSetRenderOperation(DeferredCommandBuffer::GpuOperation::kRender);
  transition(*output, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  command_processor_.PushImageMemoryBarrier(destination.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
      destination.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT, destination.current_access_mask(),
      VK_ACCESS_TRANSFER_WRITE_BIT, destination.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  destination.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  command_processor_.SubmitBarriers(true);
  for (uint32_t aspect = 0; aspect < 2; ++aspect) {
    if (aspect) {
      // NVK packs both aspects in one allocation. Explicitly order the second
      // component-masked write after depth, even though Vulkan aspects differ.
      command_processor_.PushImageMemoryBarrier(destination.image(),
          ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
      command_processor_.SubmitBarriers(true);
    }
    std::vector<VkImageCopy> copies;
    for (const auto& plan : plans) for (const auto& r : plan.rectangles) {
      VkImageCopy copy{}; copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstSubresource = {aspect ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.srcOffset = {int32_t(r.x - dst_bounds.x), int32_t(r.y - dst_bounds.y), 0};
      copy.dstOffset = {int32_t(r.x), int32_t(r.y), 0}; copy.extent = {r.width, r.height, 1}; copies.push_back(copy);
    }
    commands.CmdVkCopyImage(output->images[aspect], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
  }
  VkPipelineStageFlags stage; VkAccessFlags access; VkImageLayout layout;
  destination.GetDrawUsage(&stage, &access, &layout);
  command_processor_.PushImageMemoryBarrier(destination.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
      destination.current_stage_mask(), stage, destination.current_access_mask(), access,
      destination.current_layout(), layout);
  destination.SetUsage(stage, access, layout);
  const auto previous_objects = raw_depth_objects_;
  raw_depth_objects_ += plans.size(); raw_depth_old_regions_ += old_regions;
  if (!previous_objects || previous_objects / 256 != raw_depth_objects_ / 256) REXGPU_INFO("[RawDepthOwnership] objects={} old_regions={} last_sources={} last_rectangles={} pool_bytes={}",
      raw_depth_objects_, raw_depth_old_regions_, plans.size(), rectangle_count, raw_depth_scratch_bytes_);
  return true;
}
