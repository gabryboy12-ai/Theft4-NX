#include "graphics/gta4_native/native_pipeline_recipe.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <vector>

using Recipe = rex::graphics::gta4_native::NativePipelineRecipe;

static VKAPI_ATTR VkResult VKAPI_CALL InspectCreate(
    VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo* info,
    const VkAllocationCallbacks*, VkPipeline* pipeline) {
  REQUIRE(count == 1);
  REQUIRE(info->stageCount == 2);
  REQUIRE(info->pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT);
  REQUIRE(info->pStages[1].stage == VK_SHADER_STAGE_FRAGMENT_BIT);
  REQUIRE(info->pStages[1].module == reinterpret_cast<VkShaderModule>(2));
  REQUIRE(*static_cast<const uint32_t*>(info->pStages[1].pSpecializationInfo->pData) == 37);
  REQUIRE(info->pVertexInputState->pVertexBindingDescriptions[0].stride == 48);
  REQUIRE(info->pVertexInputState->pVertexAttributeDescriptions[0].location == 3);
  REQUIRE(info->pVertexInputState->pVertexAttributeDescriptions[0].format == VK_FORMAT_R32G32B32_SFLOAT);
  REQUIRE(info->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  REQUIRE(info->pInputAssemblyState->primitiveRestartEnable);
  REQUIRE(info->pRasterizationState->depthClampEnable);
  REQUIRE(info->pRasterizationState->depthBiasEnable);
  const auto* clip = static_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(
      info->pViewportState->pNext);
  REQUIRE(clip);
  REQUIRE(clip->negativeOneToOne);
  REQUIRE(info->pMultisampleState->rasterizationSamples == VK_SAMPLE_COUNT_4_BIT);
  REQUIRE(*info->pMultisampleState->pSampleMask == 5);
  REQUIRE(info->pDepthStencilState->front.passOp == VK_STENCIL_OP_INVERT);
  REQUIRE(info->pDepthStencilState->front.reference == 0);
  REQUIRE(info->pDepthStencilState->back.reference == 0);
  REQUIRE(info->pColorBlendState->attachmentCount == 2);
  REQUIRE(info->pColorBlendState->pAttachments[1].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
  REQUIRE(info->pColorBlendState->pAttachments[1].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
  const auto* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(info->pNext);
  REQUIRE(rendering->colorAttachmentCount == 2);
  REQUIRE(rendering->pColorAttachmentFormats[0] == VK_FORMAT_UNDEFINED);
  REQUIRE(rendering->pColorAttachmentFormats[1] == VK_FORMAT_R16G16B16A16_SFLOAT);
  REQUIRE(rendering->depthAttachmentFormat == VK_FORMAT_D32_SFLOAT_S8_UINT);
  REQUIRE(info->layout == reinterpret_cast<VkPipelineLayout>(3));
  *pipeline = reinterpret_cast<VkPipeline>(4);
  return VK_SUCCESS;
}

TEST_CASE("pipeline recipes own exact Vulkan inputs across source lifetime", "[gta4-native][pipeline-recipe]") {
  std::optional<Recipe> recipe;
  {
    uint32_t specialization_value = 37;
    VkSpecializationMapEntry specialization_entry{0, 0, sizeof(specialization_value)};
    VkSpecializationInfo specialization{1, &specialization_entry, sizeof(specialization_value),
                                        &specialization_value};
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module = reinterpret_cast<VkShaderModule>(1);
    stages[1].module = reinterpret_cast<VkShaderModule>(2);
    stages[0].pName = stages[1].pName = "shaderMain";
    stages[1].pSpecializationInfo = &specialization;
    std::vector<VkVertexInputBindingDescription> bindings{{0, 48, VK_VERTEX_INPUT_RATE_VERTEX}};
    std::vector<VkVertexInputAttributeDescription> attributes{{3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = bindings.size();
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = attributes.size();
    vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assembly.primitiveRestartEnable = VK_TRUE;
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    viewport.pNext = &clip;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = raster.depthBiasEnable = VK_TRUE;
    raster.lineWidth = 1.0f;
    VkSampleMask sample_mask = 5;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    multisample.pSampleMask = &sample_mask;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.front.passOp = VK_STENCIL_OP_INVERT;
    depth.front.reference = 99;
    depth.back.reference = 101;
    std::array<VkFormat, 2> formats{VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT};
    std::array<VkPipelineColorBlendAttachmentState, 2> attachments{};
    attachments[1].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachments[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = attachments.size();
    blend.pAttachments = attachments.data();
    std::array<VkDynamicState, 7> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = states.size();
    dynamic.pDynamicStates = states.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = formats.size();
    rendering.pColorAttachmentFormats = formats.data();
    rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = stages.size();
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = reinterpret_cast<VkPipelineLayout>(3);
    recipe = Recipe::Capture(info, specialization_value);
    REQUIRE(recipe);
    // Unknown state must reject capture, preserving the synchronous exact path.
    assembly.flags = 1;
    REQUIRE(!Recipe::Capture(info, specialization_value));
    assembly.flags = 0;
    dynamic.flags = 1;
    REQUIRE(!Recipe::Capture(info, specialization_value));
    dynamic.flags = 0;
    // Destroy and mutate every source array before submitting the saved recipe.
    bindings.front().stride = 0;
    bindings.clear();
    attributes.clear();
    specialization_value = 0;
    sample_mask = 0;
  }
  auto copied = *recipe;
  recipe.reset();
  VkPipeline pipeline = VK_NULL_HANDLE;
  REQUIRE(copied.Create(InspectCreate, VK_NULL_HANDLE, VK_NULL_HANDLE, &pipeline) == VK_SUCCESS);
  REQUIRE(pipeline == reinterpret_cast<VkPipeline>(4));
  auto invalid = copied;
  invalid.data.samples = VkSampleCountFlagBits(3);
  REQUIRE_FALSE(invalid.Valid());
  invalid = copied;
  invalid.data.rasterization.flags = 1;
  REQUIRE_FALSE(invalid.Valid());
  invalid = copied;
  invalid.data.depth_stencil.pNext = &copied;
  REQUIRE_FALSE(invalid.Valid());
  invalid = copied;
  invalid.data.color_attachments[0].colorBlendOp = VK_BLEND_OP_MAX_ENUM;
  REQUIRE_FALSE(invalid.Valid());
}
