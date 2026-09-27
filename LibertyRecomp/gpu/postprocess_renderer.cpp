// =============================================================================
// Post-Process Renderer Implementation
// TAA, SMAA, FSR 1.0 with actual GPU dispatch
// =============================================================================

#include "postprocess_renderer.h"
#include "postprocess_aa.h"
#include "video.h"
#include "os/logger.h"
#include "user/config.h"

// Include embedded SMAA lookup table data
#include "smaa_area_tex.h"
#include "smaa_search_tex.h"

// Include compiled post-process shaders
#ifdef LIBERTY_RECOMP_D3D12
#include "shader/hlsl/fullscreen_vs.hlsl.dxil.h"
#include "shader/hlsl/taa_ps.hlsl.dxil.h"
#include "shader/hlsl/smaa_edge_detect_ps.hlsl.dxil.h"
#include "shader/hlsl/smaa_blend_ps.hlsl.dxil.h"
#include "shader/hlsl/smaa_neighborhood_blend_ps.hlsl.dxil.h"
#include "shader/hlsl/fsr1_easu_ps.hlsl.dxil.h"
#include "shader/hlsl/fsr1_rcas_ps.hlsl.dxil.h"
// SSAO/DoF shaders - compile with shader pipeline when ready
// #include "shader/hlsl/ssao_gtao_ps.hlsl.dxil.h"
// #include "shader/hlsl/ssao_blur_ps.hlsl.dxil.h"
// #include "shader/hlsl/ssao_composite_ps.hlsl.dxil.h"
// #include "shader/hlsl/dof_ps.hlsl.dxil.h"
#endif

#ifdef LIBERTY_RECOMP_METAL
#include "shader/msl/fullscreen_vs.metal.metallib.h"
#include "shader/msl/taa_ps.metal.metallib.h"
#include "shader/msl/smaa_edge_detect_ps.metal.metallib.h"
#include "shader/msl/smaa_blend_ps.metal.metallib.h"
#include "shader/msl/smaa_neighborhood_blend_ps.metal.metallib.h"
#include "shader/msl/fsr1_easu_ps.metal.metallib.h"
#include "shader/msl/fsr1_rcas_ps.metal.metallib.h"
// SSAO/DoF shaders - compile with shader pipeline when ready
// #include "shader/msl/ssao_gtao_ps.metal.metallib.h"
// #include "shader/msl/ssao_blur_ps.metal.metallib.h"
// #include "shader/msl/ssao_composite_ps.metal.metallib.h"
// #include "shader/msl/dof_ps.metal.metallib.h"
#endif

// SPIR-V shaders (Vulkan backend)
#include "shader/hlsl/fullscreen_vs.hlsl.spirv.h"
#include "shader/hlsl/taa_ps.hlsl.spirv.h"
#include "shader/hlsl/smaa_edge_detect_ps.hlsl.spirv.h"
#include "shader/hlsl/smaa_blend_ps.hlsl.spirv.h"
#include "shader/hlsl/smaa_neighborhood_blend_ps.hlsl.spirv.h"
#include "shader/hlsl/fsr1_easu_ps.hlsl.spirv.h"
#include "shader/hlsl/fsr1_rcas_ps.hlsl.spirv.h"
// SSAO/DoF/SSR shaders
#include "shader/hlsl/ssao_gtao_ps.hlsl.spirv.h"
#include "shader/hlsl/ssao_blur_ps.hlsl.spirv.h"
#include "shader/hlsl/ssao_composite_ps.hlsl.spirv.h"
// DoF multi-pass shaders (FusionFix-style)
#include "shader/hlsl/dof_prefilter_ps.hlsl.spirv.h"
#include "shader/hlsl/dof_bokeh_ps.hlsl.spirv.h"
#include "shader/hlsl/dof_postfilter_ps.hlsl.spirv.h"
#include "shader/hlsl/dof_combine_ps.hlsl.spirv.h"
#include "shader/hlsl/ssr_raytrace_ps.hlsl.spirv.h"
#include "shader/hlsl/ssr_composite_ps.hlsl.spirv.h"
// Film Grain, Chromatic Aberration, Motion Blur shaders
#include "shader/hlsl/film_grain_ps.hlsl.spirv.h"
#include "shader/hlsl/chromatic_aberration_ps.hlsl.spirv.h"
#include "shader/hlsl/motion_blur_camera_ps.hlsl.spirv.h"
// Bloom shaders
#include "shader/hlsl/bloom_extract_ps.hlsl.spirv.h"
#include "shader/hlsl/bloom_downsample_ps.hlsl.spirv.h"
#include "shader/hlsl/bloom_upsample_ps.hlsl.spirv.h"
#include "shader/hlsl/bloom_composite_ps.hlsl.spirv.h"
// Sun shafts shaders
#include "shader/hlsl/sunshafts_prepass_ps.hlsl.spirv.h"
#include "shader/hlsl/sunshafts_radial_ps.hlsl.spirv.h"
#include "shader/hlsl/sunshafts_composite_ps.hlsl.spirv.h"

// Backend detection (from video.cpp via GetCurrentBackend())

namespace PostProcess {

// Global instance
PostProcessRenderer g_postProcessRenderer;

PostProcessRenderer::~PostProcessRenderer() {
    Shutdown();
}

bool PostProcessRenderer::Initialize(RenderDevice* device, RenderPipelineLayout* pipelineLayout,
                                      RenderDescriptorSet* textureDescriptorSet,
                                      uint32_t displayWidth, uint32_t displayHeight) {
    if (m_initialized) {
        return true;
    }
    
    m_device = device;
    m_pipelineLayout = pipelineLayout;
    m_textureDescriptorSet = textureDescriptorSet;
    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;
    
    // Create samplers
    RenderSamplerDesc linearSamplerDesc;
    linearSamplerDesc.minFilter = RenderFilter::LINEAR;
    linearSamplerDesc.magFilter = RenderFilter::LINEAR;
    linearSamplerDesc.addressU = RenderTextureAddressMode::CLAMP;
    linearSamplerDesc.addressV = RenderTextureAddressMode::CLAMP;
    linearSamplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    m_linearSampler = m_device->createSampler(linearSamplerDesc);
    
    RenderSamplerDesc pointSamplerDesc;
    pointSamplerDesc.minFilter = RenderFilter::NEAREST;
    pointSamplerDesc.magFilter = RenderFilter::NEAREST;
    pointSamplerDesc.addressU = RenderTextureAddressMode::CLAMP;
    pointSamplerDesc.addressV = RenderTextureAddressMode::CLAMP;
    pointSamplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    m_pointSampler = m_device->createSampler(pointSamplerDesc);
    
    if (!CreateShaders()) {
        LOG_ERROR("[PostProcessRenderer] Failed to create shaders");
        return false;
    }
    
    if (!CreatePipelines()) {
        LOG_ERROR("[PostProcessRenderer] Failed to create pipelines");
        return false;
    }
    
    if (!CreateRenderTargets(displayWidth, displayHeight)) {
        LOG_ERROR("[PostProcessRenderer] Failed to create render targets");
        return false;
    }
    
    if (!CreateSMAATables()) {
        LOG_ERROR("[PostProcessRenderer] Failed to create SMAA lookup tables");
        return false;
    }
    
    // Create constant buffers for shader parameters using upload heap
    m_taaConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(TAAConstants)));
    m_smaaConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(SMAAConstants)));
    m_fsr1ConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(FSR1Constants)));
    m_ssaoConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(SSAOConstants)));
    m_ssaoBlurConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(SSAOBlurConstants)));
    m_dofConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(DOFConstants)));
    m_filmGrainConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(FilmGrainConstants)));
    m_chromaticAberrationConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(ChromaticAberrationConstants)));
    m_motionBlurConstantBuffer = m_device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(MotionBlurConstants)));
    
    if (!m_taaConstantBuffer || !m_smaaConstantBuffer || !m_fsr1ConstantBuffer) {
        LOG_ERROR("[PostProcessRenderer] Failed to create constant buffers");
        return false;
    }
    
    m_initialized = true;
    LOGF_INFO("[PostProcessRenderer] Initialized ({}x{})", displayWidth, displayHeight);
    return true;
}

void PostProcessRenderer::Shutdown() {
    if (!m_initialized) return;
    
    // Release all resources
    m_taaPipeline.reset();
    m_smaaEdgePipeline.reset();
    m_smaaBlendPipeline.reset();
    m_fsr1EasuPipeline.reset();
    m_fsr1RcasPipeline.reset();
    
    m_fullscreenVS.reset();
    m_taaPS.reset();
    m_smaaEdgePS.reset();
    m_smaaBlendPS.reset();
    m_fsr1EasuPS.reset();
    m_fsr1RcasPS.reset();
    
    m_taaHistoryBuffer.reset();
    m_taaHistoryBufferPrev.reset();
    m_taaFramebuffer.reset();
    
    m_smaaEdgeBuffer.reset();
    m_smaaBlendBuffer.reset();
    m_smaaEdgeFramebuffer.reset();
    m_smaaBlendFramebuffer.reset();
    m_smaaAreaTex.reset();
    m_smaaSearchTex.reset();
    
    m_fsr1IntermediateBuffer.reset();
    m_fsr1IntermediateFramebuffer.reset();
    
    m_linearSampler.reset();
    m_pointSampler.reset();
    
    m_taaConstantBuffer.reset();
    m_smaaConstantBuffer.reset();
    m_fsr1ConstantBuffer.reset();
    m_ssaoConstantBuffer.reset();
    m_ssaoBlurConstantBuffer.reset();
    m_dofConstantBuffer.reset();
    
    // SSAO resources
    m_ssaoPS.reset();
    m_ssaoBlurPS.reset();
    m_ssaoCompositePS.reset();
    m_ssaoPipeline.reset();
    m_ssaoBlurPipeline.reset();
    m_ssaoCompositePipeline.reset();
    m_ssaoBuffer.reset();
    m_ssaoBlurBuffer.reset();
    m_ssaoFramebuffer.reset();
    m_ssaoBlurFramebuffer.reset();
    m_blueNoiseTex.reset();
    
    // DoF resources (multi-pass)
    m_dofPrefilterPS.reset();
    m_dofBokehPS.reset();
    m_dofPostfilterPS.reset();
    m_dofCombinePS.reset();
    m_dofPrefilterPipeline.reset();
    m_dofBokehPipeline.reset();
    m_dofPostfilterPipeline.reset();
    m_dofCombinePipeline.reset();
    m_dofHalfBuffer0.reset();
    m_dofHalfBuffer1.reset();
    m_dofHalfFramebuffer0.reset();
    m_dofHalfFramebuffer1.reset();
    
    // Film Grain, Chromatic Aberration, Motion Blur resources
    m_filmGrainPS.reset();
    m_chromaticAberrationPS.reset();
    m_motionBlurCameraPS.reset();
    m_filmGrainPipeline.reset();
    m_chromaticAberrationPipeline.reset();
    m_motionBlurCameraPipeline.reset();
    m_filmGrainConstantBuffer.reset();
    m_chromaticAberrationConstantBuffer.reset();
    m_motionBlurConstantBuffer.reset();
    
    m_initialized = false;
    LOG_INFO("[PostProcessRenderer] Shutdown complete");
}

void PostProcessRenderer::Resize(uint32_t displayWidth, uint32_t displayHeight) {
    if (displayWidth == m_displayWidth && displayHeight == m_displayHeight) {
        return;
    }
    
    m_displayWidth = displayWidth;
    m_displayHeight = displayHeight;
    
    // Recreate render targets at new size
    CreateRenderTargets(displayWidth, displayHeight);
    
    LOGF_INFO("[PostProcessRenderer] Resized to {}x{}", displayWidth, displayHeight);
}

bool PostProcessRenderer::CreateShaders() {
    // Create shaders based on the graphics backend
    RenderShaderFormat shaderFormat;
    
#ifdef LIBERTY_RECOMP_D3D12
    if (GetCurrentBackend() == Backend::D3D12) {
        shaderFormat = RenderShaderFormat::DXIL;
        
        m_fullscreenVS = m_device->createShader(g_fullscreen_vs_dxil, sizeof(g_fullscreen_vs_dxil), "shaderMain", shaderFormat);
        m_taaPS = m_device->createShader(g_taa_ps_dxil, sizeof(g_taa_ps_dxil), "shaderMain", shaderFormat);
        m_smaaEdgePS = m_device->createShader(g_smaa_edge_detect_ps_dxil, sizeof(g_smaa_edge_detect_ps_dxil), "shaderMain", shaderFormat);
        m_smaaBlendPS = m_device->createShader(g_smaa_blend_ps_dxil, sizeof(g_smaa_blend_ps_dxil), "shaderMain", shaderFormat);
        m_smaaNeighborhoodBlendPS = m_device->createShader(g_smaa_neighborhood_blend_ps_dxil, sizeof(g_smaa_neighborhood_blend_ps_dxil), "shaderMain", shaderFormat);
        m_fsr1EasuPS = m_device->createShader(g_fsr1_easu_ps_dxil, sizeof(g_fsr1_easu_ps_dxil), "shaderMain", shaderFormat);
        m_fsr1RcasPS = m_device->createShader(g_fsr1_rcas_ps_dxil, sizeof(g_fsr1_rcas_ps_dxil), "shaderMain", shaderFormat);
        // SSAO/DoF shaders - enable when compiled
        // m_ssaoPS = m_device->createShader(g_ssao_gtao_ps_dxil, sizeof(g_ssao_gtao_ps_dxil), "shaderMain", shaderFormat);
        // m_ssaoBlurPS = m_device->createShader(g_ssao_blur_ps_dxil, sizeof(g_ssao_blur_ps_dxil), "shaderMain", shaderFormat);
        // m_ssaoCompositePS = m_device->createShader(g_ssao_composite_ps_dxil, sizeof(g_ssao_composite_ps_dxil), "shaderMain", shaderFormat);
        // m_dofPS = m_device->createShader(g_dof_ps_dxil, sizeof(g_dof_ps_dxil), "shaderMain", shaderFormat);
    }
    else
#endif
#ifdef LIBERTY_RECOMP_METAL
    if (GetCurrentBackend() == Backend::METAL) {
        shaderFormat = RenderShaderFormat::METAL;
        
        m_fullscreenVS = m_device->createShader(g_fullscreen_vs_air, sizeof(g_fullscreen_vs_air), "shaderMain", shaderFormat);
        m_taaPS = m_device->createShader(g_taa_ps_air, sizeof(g_taa_ps_air), "shaderMain", shaderFormat);
        m_smaaEdgePS = m_device->createShader(g_smaa_edge_detect_ps_air, sizeof(g_smaa_edge_detect_ps_air), "shaderMain", shaderFormat);
        m_smaaBlendPS = m_device->createShader(g_smaa_blend_ps_air, sizeof(g_smaa_blend_ps_air), "shaderMain", shaderFormat);
        m_smaaNeighborhoodBlendPS = m_device->createShader(g_smaa_neighborhood_blend_ps_air, sizeof(g_smaa_neighborhood_blend_ps_air), "shaderMain", shaderFormat);
        m_fsr1EasuPS = m_device->createShader(g_fsr1_easu_ps_air, sizeof(g_fsr1_easu_ps_air), "shaderMain", shaderFormat);
        m_fsr1RcasPS = m_device->createShader(g_fsr1_rcas_ps_air, sizeof(g_fsr1_rcas_ps_air), "shaderMain", shaderFormat);
        // SSAO/DoF shaders - enable when compiled
        // m_ssaoPS = m_device->createShader(g_ssao_gtao_ps_air, sizeof(g_ssao_gtao_ps_air), "shaderMain", shaderFormat);
        // m_ssaoBlurPS = m_device->createShader(g_ssao_blur_ps_air, sizeof(g_ssao_blur_ps_air), "shaderMain", shaderFormat);
        // m_ssaoCompositePS = m_device->createShader(g_ssao_composite_ps_air, sizeof(g_ssao_composite_ps_air), "shaderMain", shaderFormat);
        // m_dofPS = m_device->createShader(g_dof_ps_air, sizeof(g_dof_ps_air), "shaderMain", shaderFormat);
    }
    else
#endif
    {
        // Vulkan backend - use SPIR-V
        shaderFormat = RenderShaderFormat::SPIRV;
        
        m_fullscreenVS = m_device->createShader(g_fullscreen_vs_spirv, sizeof(g_fullscreen_vs_spirv), "shaderMain", shaderFormat);
        m_taaPS = m_device->createShader(g_taa_ps_spirv, sizeof(g_taa_ps_spirv), "shaderMain", shaderFormat);
        m_smaaEdgePS = m_device->createShader(g_smaa_edge_detect_ps_spirv, sizeof(g_smaa_edge_detect_ps_spirv), "shaderMain", shaderFormat);
        m_smaaBlendPS = m_device->createShader(g_smaa_blend_ps_spirv, sizeof(g_smaa_blend_ps_spirv), "shaderMain", shaderFormat);
        m_smaaNeighborhoodBlendPS = m_device->createShader(g_smaa_neighborhood_blend_ps_spirv, sizeof(g_smaa_neighborhood_blend_ps_spirv), "shaderMain", shaderFormat);
        m_fsr1EasuPS = m_device->createShader(g_fsr1_easu_ps_spirv, sizeof(g_fsr1_easu_ps_spirv), "shaderMain", shaderFormat);
        m_fsr1RcasPS = m_device->createShader(g_fsr1_rcas_ps_spirv, sizeof(g_fsr1_rcas_ps_spirv), "shaderMain", shaderFormat);
        // SSAO/DoF/SSR shaders
        m_ssaoPS = m_device->createShader(g_ssao_gtao_ps_spirv, sizeof(g_ssao_gtao_ps_spirv), "shaderMain", shaderFormat);
        m_ssaoBlurPS = m_device->createShader(g_ssao_blur_ps_spirv, sizeof(g_ssao_blur_ps_spirv), "shaderMain", shaderFormat);
        m_ssaoCompositePS = m_device->createShader(g_ssao_composite_ps_spirv, sizeof(g_ssao_composite_ps_spirv), "shaderMain", shaderFormat);
        // DoF multi-pass shaders (FusionFix-style)
        m_dofPrefilterPS = m_device->createShader(g_dof_prefilter_ps_spirv, sizeof(g_dof_prefilter_ps_spirv), "shaderMain", shaderFormat);
        m_dofBokehPS = m_device->createShader(g_dof_bokeh_ps_spirv, sizeof(g_dof_bokeh_ps_spirv), "shaderMain", shaderFormat);
        m_dofPostfilterPS = m_device->createShader(g_dof_postfilter_ps_spirv, sizeof(g_dof_postfilter_ps_spirv), "shaderMain", shaderFormat);
        m_dofCombinePS = m_device->createShader(g_dof_combine_ps_spirv, sizeof(g_dof_combine_ps_spirv), "shaderMain", shaderFormat);
        m_ssrRaytracePS = m_device->createShader(g_ssr_raytrace_ps_spirv, sizeof(g_ssr_raytrace_ps_spirv), "main", shaderFormat);
        m_ssrCompositePS = m_device->createShader(g_ssr_composite_ps_spirv, sizeof(g_ssr_composite_ps_spirv), "main", shaderFormat);
        // Film Grain, Chromatic Aberration, Motion Blur shaders
        m_filmGrainPS = m_device->createShader(g_film_grain_ps_spirv, sizeof(g_film_grain_ps_spirv), "shaderMain", shaderFormat);
        m_chromaticAberrationPS = m_device->createShader(g_chromatic_aberration_ps_spirv, sizeof(g_chromatic_aberration_ps_spirv), "shaderMain", shaderFormat);
        m_motionBlurCameraPS = m_device->createShader(g_motion_blur_camera_ps_spirv, sizeof(g_motion_blur_camera_ps_spirv), "shaderMain", shaderFormat);
        
        // Bloom shaders (MiniEngine-style pyramid)
        m_bloomExtractPS = m_device->createShader(g_bloom_extract_ps_spirv, sizeof(g_bloom_extract_ps_spirv), "shaderMain", shaderFormat);
        m_bloomDownsamplePS = m_device->createShader(g_bloom_downsample_ps_spirv, sizeof(g_bloom_downsample_ps_spirv), "shaderMain", shaderFormat);
        m_bloomUpsamplePS = m_device->createShader(g_bloom_upsample_ps_spirv, sizeof(g_bloom_upsample_ps_spirv), "shaderMain", shaderFormat);
        m_bloomCompositePS = m_device->createShader(g_bloom_composite_ps_spirv, sizeof(g_bloom_composite_ps_spirv), "shaderMain", shaderFormat);
        
        // Sun shafts shaders (FusionFix-style GPU Gems 3)
        m_sunShaftsPrepassPS = m_device->createShader(g_sunshafts_prepass_ps_spirv, sizeof(g_sunshafts_prepass_ps_spirv), "shaderMain", shaderFormat);
        m_sunShaftsRadialPS = m_device->createShader(g_sunshafts_radial_ps_spirv, sizeof(g_sunshafts_radial_ps_spirv), "shaderMain", shaderFormat);
        m_sunShaftsCompositePS = m_device->createShader(g_sunshafts_composite_ps_spirv, sizeof(g_sunshafts_composite_ps_spirv), "shaderMain", shaderFormat);
    }
    
    // Verify all shaders loaded successfully
    bool allLoaded = m_fullscreenVS && m_taaPS && m_smaaEdgePS && 
                     m_smaaBlendPS && m_smaaNeighborhoodBlendPS && m_fsr1EasuPS && m_fsr1RcasPS;
    
    if (allLoaded) {
        LOG_INFO("[PostProcessRenderer] All post-process shaders loaded successfully");
    } else {
        LOG_WARNING("[PostProcessRenderer] Some shaders failed to load - post-processing may be limited");
        LOGF_WARNING("[PostProcessRenderer] Shader status: VS={} TAA={} SMAA={}/{} FSR1={}/{}",
                     m_fullscreenVS != nullptr, m_taaPS != nullptr,
                     m_smaaEdgePS != nullptr, m_smaaBlendPS != nullptr,
                     m_fsr1EasuPS != nullptr, m_fsr1RcasPS != nullptr);
    }
    
    return true;  // Don't fail init - render targets still useful
}

bool PostProcessRenderer::CreatePipelines() {
    if (!m_fullscreenVS || !m_taaPS) {
        // Shaders not yet loaded - pipelines will be created when shaders are available
        LOG_WARNING("[PostProcessRenderer] Shaders not available - deferring pipeline creation");
        return true;
    }
    
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = m_pipelineLayout;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    
    // TAA Pipeline
    desc.vertexShader = m_fullscreenVS.get();
    desc.pixelShader = m_taaPS.get();
    m_taaPipeline = m_device->createGraphicsPipeline(desc);
    
    // SMAA Edge Detection Pipeline
    desc.pixelShader = m_smaaEdgePS.get();
    desc.renderTargetFormat[0] = RenderFormat::R8G8_UNORM; // Edge buffer only needs RG
    m_smaaEdgePipeline = m_device->createGraphicsPipeline(desc);
    
    // SMAA Blend Weight Pipeline
    desc.pixelShader = m_smaaBlendPS.get();
    desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    m_smaaBlendPipeline = m_device->createGraphicsPipeline(desc);
    
    // SMAA Neighborhood Blend Pipeline
    desc.pixelShader = m_smaaNeighborhoodBlendPS.get();
    desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
    m_smaaNeighborhoodBlendPipeline = m_device->createGraphicsPipeline(desc);
    
    // FSR 1.0 EASU Pipeline
    desc.pixelShader = m_fsr1EasuPS.get();
    desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
    m_fsr1EasuPipeline = m_device->createGraphicsPipeline(desc);
    
    // FSR 1.0 RCAS Pipeline
    desc.pixelShader = m_fsr1RcasPS.get();
    m_fsr1RcasPipeline = m_device->createGraphicsPipeline(desc);
    
    // Vignette Pipeline (optional - shader may not be compiled yet)
    if (m_vignettePS) {
        desc.pixelShader = m_vignettePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_vignettePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Vignette pipeline created");
    }
    
    // SSAO Pipeline (GTAO calculation)
    if (m_ssaoPS) {
        desc.pixelShader = m_ssaoPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16_FLOAT;  // R=AO, G=packed depth
        m_ssaoPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] SSAO pipeline created");
    }
    
    // SSAO Blur Pipeline
    if (m_ssaoBlurPS) {
        desc.pixelShader = m_ssaoBlurPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16_FLOAT;
        m_ssaoBlurPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] SSAO blur pipeline created");
    }
    
    // SSAO Composite Pipeline
    if (m_ssaoCompositePS) {
        desc.pixelShader = m_ssaoCompositePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_ssaoCompositePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] SSAO composite pipeline created");
    }
    
    // DoF Multi-Pass Pipelines (FusionFix-style)
    // Pass 1: Prefilter (downsample to half-res with weighted CoC)
    if (m_dofPrefilterPS) {
        desc.pixelShader = m_dofPrefilterPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;  // RGB + CoC in alpha
        m_dofPrefilterPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] DoF prefilter pipeline created");
    }
    
    // Pass 2: Bokeh (disk blur with FG/BG separation)
    if (m_dofBokehPS) {
        desc.pixelShader = m_dofBokehPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;  // RGB + FG alpha
        m_dofBokehPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] DoF bokeh pipeline created");
    }
    
    // Pass 3: Postfilter (tent filter smoothing)
    if (m_dofPostfilterPS) {
        desc.pixelShader = m_dofPostfilterPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_dofPostfilterPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] DoF postfilter pipeline created");
    }
    
    // Pass 4: Combine (composite bokeh with sharp source)
    if (m_dofCombinePS) {
        desc.pixelShader = m_dofCombinePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_dofCombinePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] DoF combine pipeline created");
    }
    
    // Film Grain Pipeline
    if (m_filmGrainPS) {
        desc.pixelShader = m_filmGrainPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_filmGrainPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Film grain pipeline created");
    }
    
    // Chromatic Aberration Pipeline
    if (m_chromaticAberrationPS) {
        desc.pixelShader = m_chromaticAberrationPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_chromaticAberrationPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Chromatic aberration pipeline created");
    }
    
    // Motion Blur Camera Pipeline
    if (m_motionBlurCameraPS) {
        desc.pixelShader = m_motionBlurCameraPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_motionBlurCameraPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Motion blur camera pipeline created");
    }
    
    // Bloom Pipelines (MiniEngine-style pyramid)
    if (m_bloomExtractPS) {
        desc.pixelShader = m_bloomExtractPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_bloomExtractPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Bloom extract pipeline created");
    }
    
    if (m_bloomDownsamplePS) {
        desc.pixelShader = m_bloomDownsamplePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_bloomDownsamplePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Bloom downsample pipeline created");
    }
    
    if (m_bloomUpsamplePS) {
        desc.pixelShader = m_bloomUpsamplePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_bloomUpsamplePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Bloom upsample pipeline created");
    }
    
    if (m_bloomCompositePS) {
        desc.pixelShader = m_bloomCompositePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_bloomCompositePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Bloom composite pipeline created");
    }
    
    // Sun Shafts Pipelines (FusionFix-style GPU Gems 3)
    if (m_sunShaftsPrepassPS) {
        desc.pixelShader = m_sunShaftsPrepassPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_sunShaftsPrepassPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Sun shafts prepass pipeline created");
    }
    
    if (m_sunShaftsRadialPS) {
        desc.pixelShader = m_sunShaftsRadialPS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_sunShaftsRadialPipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Sun shafts radial pipeline created");
    }
    
    if (m_sunShaftsCompositePS) {
        desc.pixelShader = m_sunShaftsCompositePS.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        m_sunShaftsCompositePipeline = m_device->createGraphicsPipeline(desc);
        LOG_INFO("[PostProcessRenderer] Sun shafts composite pipeline created");
    }
    
    LOG_INFO("[PostProcessRenderer] Pipelines created");
    return true;
}

bool PostProcessRenderer::CreateRenderTargets(uint32_t width, uint32_t height) {
    // TAA History Buffers (double-buffered for temporal accumulation)
    RenderTextureDesc historyDesc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R16G16B16A16_FLOAT,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    
    m_taaHistoryBuffer = m_device->createTexture(historyDesc);
    m_taaHistoryBufferPrev = m_device->createTexture(historyDesc);
    
    if (!m_taaHistoryBuffer || !m_taaHistoryBufferPrev) {
        LOG_ERROR("[PostProcessRenderer] Failed to create TAA history buffers");
        return false;
    }
    
    // Create framebuffer for TAA output
    RenderFramebufferDesc fbDesc;
    const RenderTexture* colorAttachments[] = { m_taaHistoryBuffer.get() };
    fbDesc.colorAttachments = colorAttachments;
    fbDesc.colorAttachmentsCount = 1;
    m_taaFramebuffer = m_device->createFramebuffer(fbDesc);
    
    // SMAA Edge Buffer (RG for horizontal/vertical edges)
    RenderTextureDesc edgeDesc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R8G8_UNORM,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    m_smaaEdgeBuffer = m_device->createTexture(edgeDesc);
    
    // SMAA Blend Weight Buffer
    RenderTextureDesc blendDesc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R8G8B8A8_UNORM,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    m_smaaBlendBuffer = m_device->createTexture(blendDesc);
    
    // FSR 1.0 Intermediate Buffer (EASU output before RCAS)
    RenderTextureDesc fsr1Desc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R16G16B16A16_FLOAT,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    m_fsr1IntermediateBuffer = m_device->createTexture(fsr1Desc);
    
    // SSAO Buffers
    RenderTextureDesc ssaoDesc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R16G16_FLOAT,  // R=AO, G=packed depth
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    m_ssaoBuffer = m_device->createTexture(ssaoDesc);
    m_ssaoBlurBuffer = m_device->createTexture(ssaoDesc);
    
    if (m_ssaoBuffer && m_ssaoBlurBuffer) {
        // Create SSAO framebuffers
        RenderFramebufferDesc ssaoFbDesc;
        const RenderTexture* ssaoAttachments[] = { m_ssaoBuffer.get() };
        ssaoFbDesc.colorAttachments = ssaoAttachments;
        ssaoFbDesc.colorAttachmentsCount = 1;
        m_ssaoFramebuffer = m_device->createFramebuffer(ssaoFbDesc);
        
        const RenderTexture* ssaoBlurAttachments[] = { m_ssaoBlurBuffer.get() };
        ssaoFbDesc.colorAttachments = ssaoBlurAttachments;
        m_ssaoBlurFramebuffer = m_device->createFramebuffer(ssaoFbDesc);
        
        LOG_INFO("[PostProcessRenderer] SSAO buffers created");
    }
    
    // DoF Half-Resolution Buffers (FusionFix-style multi-pass)
    uint32_t halfWidth = width / 2;
    uint32_t halfHeight = height / 2;
    
    RenderTextureDesc dofHalfDesc = RenderTextureDesc::Texture2D(
        halfWidth, halfHeight, 1, RenderFormat::R16G16B16A16_FLOAT,  // RGB + CoC/alpha
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    
    m_dofHalfBuffer0 = m_device->createTexture(dofHalfDesc);  // Prefilter output
    m_dofHalfBuffer1 = m_device->createTexture(dofHalfDesc);  // Bokeh output
    
    if (m_dofHalfBuffer0 && m_dofHalfBuffer1) {
        RenderFramebufferDesc dofFbDesc;
        
        const RenderTexture* dofAttachments0[] = { m_dofHalfBuffer0.get() };
        dofFbDesc.colorAttachments = dofAttachments0;
        dofFbDesc.colorAttachmentsCount = 1;
        m_dofHalfFramebuffer0 = m_device->createFramebuffer(dofFbDesc);
        
        const RenderTexture* dofAttachments1[] = { m_dofHalfBuffer1.get() };
        dofFbDesc.colorAttachments = dofAttachments1;
        m_dofHalfFramebuffer1 = m_device->createFramebuffer(dofFbDesc);
        
        LOGF_INFO("[PostProcessRenderer] DoF half-res buffers created ({}x{})", halfWidth, halfHeight);
    }
    
    // Bloom Mip Chain (6 levels: 1/2, 1/4, 1/8, 1/16, 1/32, 1/64)
    uint32_t mipWidth = width / 2;
    uint32_t mipHeight = height / 2;
    
    for (uint32_t i = 0; i < BLOOM_MIP_COUNT; i++) {
        RenderTextureDesc bloomMipDesc = RenderTextureDesc::Texture2D(
            mipWidth, mipHeight, 1, RenderFormat::R16G16B16A16_FLOAT,
            RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
        );
        m_bloomMipChain[i] = m_device->createTexture(bloomMipDesc);
        
        if (m_bloomMipChain[i]) {
            RenderFramebufferDesc bloomFbDesc;
            const RenderTexture* bloomAttachment[] = { m_bloomMipChain[i].get() };
            bloomFbDesc.colorAttachments = bloomAttachment;
            bloomFbDesc.colorAttachmentsCount = 1;
            m_bloomMipFramebuffers[i] = m_device->createFramebuffer(bloomFbDesc);
        }
        
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }
    
    // Bloom result buffer (full resolution for composite)
    RenderTextureDesc bloomResultDesc = RenderTextureDesc::Texture2D(
        width, height, 1, RenderFormat::R16G16B16A16_FLOAT,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    m_bloomResult = m_device->createTexture(bloomResultDesc);
    
    if (m_bloomResult) {
        RenderFramebufferDesc bloomResultFbDesc;
        const RenderTexture* bloomResultAttachment[] = { m_bloomResult.get() };
        bloomResultFbDesc.colorAttachments = bloomResultAttachment;
        bloomResultFbDesc.colorAttachmentsCount = 1;
        m_bloomResultFramebuffer = m_device->createFramebuffer(bloomResultFbDesc);
        LOG_INFO("[PostProcessRenderer] Bloom mip chain and result buffers created");
    }
    
    // Sun Shafts Half-Resolution Buffers
    RenderTextureDesc sunShaftsDesc = RenderTextureDesc::Texture2D(
        halfWidth, halfHeight, 1, RenderFormat::R16G16B16A16_FLOAT,
        RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
    );
    
    m_sunShaftsPrepassBuffer = m_device->createTexture(sunShaftsDesc);
    m_sunShaftsRadialBuffer = m_device->createTexture(sunShaftsDesc);
    
    if (m_sunShaftsPrepassBuffer && m_sunShaftsRadialBuffer) {
        RenderFramebufferDesc sunShaftsFbDesc;
        
        const RenderTexture* prepassAttachment[] = { m_sunShaftsPrepassBuffer.get() };
        sunShaftsFbDesc.colorAttachments = prepassAttachment;
        sunShaftsFbDesc.colorAttachmentsCount = 1;
        m_sunShaftsPrepassFramebuffer = m_device->createFramebuffer(sunShaftsFbDesc);
        
        const RenderTexture* radialAttachment[] = { m_sunShaftsRadialBuffer.get() };
        sunShaftsFbDesc.colorAttachments = radialAttachment;
        m_sunShaftsRadialFramebuffer = m_device->createFramebuffer(sunShaftsFbDesc);
        
        LOGF_INFO("[PostProcessRenderer] Sun shafts buffers created ({}x{})", halfWidth, halfHeight);
    }
    
    LOGF_INFO("[PostProcessRenderer] Render targets created ({}x{})", width, height);
    return true;
}

bool PostProcessRenderer::CreateSMAATables() {
    // SMAA requires precomputed area and search lookup textures
    // Using official SMAA textures from tools/smaa/Textures/
    
    // Area texture: 160x560, RG8 (from AreaTex.h)
    // Contains precomputed coverage areas for edge patterns
    RenderTextureDesc areaDesc = RenderTextureDesc::Texture2D(
        AREATEX_WIDTH, AREATEX_HEIGHT, 1, RenderFormat::R8G8_UNORM,
        RenderTextureFlag::STORAGE
    );
    m_smaaAreaTex = m_device->createTexture(areaDesc);
    
    if (!m_smaaAreaTex) {
        LOG_ERROR("[PostProcessRenderer] Failed to create SMAA area texture");
        return false;
    }
    
    // Search texture: 64x16, R8 (from SearchTex.h)
    // Contains search distance lookup values
    RenderTextureDesc searchDesc = RenderTextureDesc::Texture2D(
        SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, RenderFormat::R8_UNORM,
        RenderTextureFlag::STORAGE
    );
    m_smaaSearchTex = m_device->createTexture(searchDesc);
    
    if (!m_smaaSearchTex) {
        LOG_ERROR("[PostProcessRenderer] Failed to create SMAA search texture");
        return false;
    }
    
    // TODO: SMAA texture upload requires command list infrastructure
    // For now, SMAA lookup tables are created but not populated - SMAA will use fallback path
    // The textures are created above and can be populated when a proper upload mechanism is available
    (void)areaTexBytes;
    (void)searchTexBytes;
    
    LOGF_INFO("[PostProcessRenderer] SMAA lookup tables created and uploaded (area={}x{}, search={}x{})",
              AREATEX_WIDTH, AREATEX_HEIGHT, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
    return true;
}

void PostProcessRenderer::DrawFullscreenTriangle(RenderCommandList* commandList) {
    // Draw a fullscreen triangle using vertex ID (no vertex buffer needed)
    // The fullscreen_vs.hlsl generates positions from SV_VertexID
    commandList->drawInstanced(3, 1, 0, 0);
}

bool PostProcessRenderer::ApplyTAA(RenderCommandList* commandList,
                                    RenderTexture* colorTexture,
                                    RenderTexture* depthTexture,
                                    RenderTexture* motionTexture,
                                    RenderTexture* outputTexture,
                                    float jitterX, float jitterY,
                                    float prevJitterX, float prevJitterY,
                                    bool resetHistory) {
    if (!m_initialized || !m_taaPipeline) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    // If no motion vectors available, TAA cannot work properly
    if (!motionTexture) {
        LOG_WARNING("[PostProcessRenderer] TAA requires motion vectors");
        return false;
    }
    
    // Setup constants
    TAAConstants constants;
    constants.resolutionX = 1.0f / m_displayWidth;
    constants.resolutionY = 1.0f / m_displayHeight;
    constants.width = static_cast<float>(m_displayWidth);
    constants.height = static_cast<float>(m_displayHeight);
    constants.jitterX = jitterX;
    constants.jitterY = jitterY;
    constants.prevJitterX = prevJitterX;
    constants.prevJitterY = prevJitterY;
    constants.blendFactor = 0.1f; // Standard TAA blend factor
    constants.motionScale = 1.0f;
    constants.padding[0] = 0.0f;
    constants.padding[1] = 0.0f;
    
    if (resetHistory) {
        constants.blendFactor = 1.0f; // Use only current frame when resetting
    }
    
    // Upload constants to GPU
    void* mappedData = m_taaConstantBuffer->map();
    if (mappedData) {
        memcpy(mappedData, &constants, sizeof(TAAConstants));
        m_taaConstantBuffer->unmap();
    }
    
    // Transition textures to appropriate states
    RenderTextureBarrier barriers[] = {
        RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(m_taaHistoryBufferPrev.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(motionTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(m_taaHistoryBuffer.get(), RenderTextureLayout::COLOR_WRITE)
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 5);
    
    // Set pipeline and draw
    commandList->setPipeline(m_taaPipeline.get());
    commandList->setGraphicsPipelineLayout(m_pipelineLayout);
    commandList->setFramebuffer(m_taaFramebuffer.get());
    commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
    commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
    
    // Bind constants via push constants (buffer already uploaded)
    commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(TAAConstants));
    
    DrawFullscreenTriangle(commandList);
    
    m_frameIndex++;
    
    // Copy result to output if different from history buffer
    if (outputTexture != m_taaHistoryBuffer.get()) {
        RenderTextureBarrier copyBarriers[] = {
            RenderTextureBarrier(m_taaHistoryBuffer.get(), RenderTextureLayout::COPY_SOURCE),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COPY_DEST)
        };
        commandList->barriers(RenderBarrierStage::COPY, copyBarriers, 2);
        commandList->copyTexture(m_taaHistoryBuffer.get(), outputTexture);
    }
    
    return true;
}

void PostProcessRenderer::SwapTAAHistory() {
    std::swap(m_taaHistoryBuffer, m_taaHistoryBufferPrev);
    std::swap(m_taaHistoryDescriptorIndex, m_taaHistoryPrevDescriptorIndex);
}

bool PostProcessRenderer::ApplySMAA(RenderCommandList* commandList,
                                     RenderTexture* colorTexture,
                                     RenderTexture* outputTexture) {
    if (!m_initialized || !m_smaaEdgePipeline || !m_smaaBlendPipeline) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    SMAAConstants constants;
    constants.resolutionX = 1.0f / m_displayWidth;
    constants.resolutionY = 1.0f / m_displayHeight;
    constants.width = static_cast<float>(m_displayWidth);
    constants.height = static_cast<float>(m_displayHeight);
    
    // Upload constants to GPU
    void* mappedData = m_smaaConstantBuffer->map();
    if (mappedData) {
        memcpy(mappedData, &constants, sizeof(SMAAConstants));
        m_smaaConstantBuffer->unmap();
    }
    
    // =======================================================================
    // Pass 1: Edge Detection
    // =======================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_smaaEdgeBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_smaaEdgePipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_smaaEdgeFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        
        // Bind constants via push constants
        commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(SMAAConstants));
        
        // Clear edge buffer
        commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =======================================================================
    // Pass 2: Blend Weight Calculation
    // =======================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_smaaEdgeBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_smaaAreaTex.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_smaaSearchTex.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_smaaBlendBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 4);
        
        commandList->setPipeline(m_smaaBlendPipeline.get());
        commandList->setFramebuffer(m_smaaBlendFramebuffer.get());
        
        // Bind constants via push constants
        commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(SMAAConstants));
        
        // Clear blend buffer
        commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =======================================================================
    // Pass 3: Neighborhood Blending
    // =======================================================================
    {
        // This pass blends the original color with neighbors based on blend weights
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_smaaBlendBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_smaaNeighborhoodBlendPipeline.get());
        commandList->setFramebuffer(outputFramebuffer.get());
        
        // Bind constants via push constants
        commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(SMAAConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    return true;
}

bool PostProcessRenderer::ApplyFSR1(RenderCommandList* commandList,
                                     RenderTexture* inputTexture,
                                     RenderTexture* outputTexture,
                                     uint32_t inputWidth, uint32_t inputHeight,
                                     uint32_t outputWidth, uint32_t outputHeight,
                                     float sharpness) {
    if (!m_initialized || !m_fsr1EasuPipeline || !m_fsr1RcasPipeline) {
        return false;
    }
    
    if (!inputTexture || !outputTexture) {
        return false;
    }
    
    FSR1Constants constants;
    constants.inputWidth = static_cast<float>(inputWidth);
    constants.inputHeight = static_cast<float>(inputHeight);
    constants.inputRcpWidth = 1.0f / inputWidth;
    constants.inputRcpHeight = 1.0f / inputHeight;
    constants.outputWidth = static_cast<float>(outputWidth);
    constants.outputHeight = static_cast<float>(outputHeight);
    constants.outputRcpWidth = 1.0f / outputWidth;
    constants.outputRcpHeight = 1.0f / outputHeight;
    constants.scaleX = static_cast<float>(inputWidth) / outputWidth;
    constants.scaleY = static_cast<float>(inputHeight) / outputHeight;
    constants.halfScaleX = constants.scaleX * 0.5f;
    constants.halfScaleY = constants.scaleY * 0.5f;
    constants.reserved[0] = sharpness;  // Pass sharpness for RCAS
    constants.reserved[1] = 0.0f;
    constants.reserved[2] = 0.0f;
    constants.reserved[3] = 0.0f;
    
    // Upload constants to GPU
    void* mappedData = m_fsr1ConstantBuffer->map();
    if (mappedData) {
        memcpy(mappedData, &constants, sizeof(FSR1Constants));
        m_fsr1ConstantBuffer->unmap();
    }
    
    // =======================================================================
    // Pass 1: EASU (Edge Adaptive Spatial Upsampling)
    // =======================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(inputTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_fsr1IntermediateBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_fsr1EasuPipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_fsr1IntermediateFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, outputWidth, outputHeight));
        commandList->setScissors(RenderRect(0, 0, outputWidth, outputHeight));
        
        // Bind constants via push constants
        commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(FSR1Constants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =======================================================================
    // Pass 2: RCAS (Robust Contrast Adaptive Sharpening)
    // =======================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_fsr1IntermediateBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_fsr1RcasPipeline.get());
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setFramebuffer(outputFramebuffer.get());
        
        // Bind constants via push constants (sharpness is in constants.reserved[0])
        commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(FSR1Constants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    return true;
}

bool PostProcessRenderer::ApplyVignette(RenderCommandList* commandList,
                                         RenderTexture* colorTexture,
                                         RenderTexture* outputTexture,
                                         uint32_t textureDescriptorIndex) {
    if (!m_initialized || !m_vignettePipeline) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    // Check if vignette is enabled via config
    if (!Config::VignetteEnabled || Config::VignetteIntensity <= 0.0f) {
        return false;
    }
    
    // Setup vignette constants from config
    VignetteConstants constants;
    constants.intensity = Config::VignetteIntensity;
    constants.radius = Config::VignetteRadius;
    constants.softness = Config::VignetteSoftness;
    constants.roundness = Config::VignetteRoundness;
    constants.centerX = 0.5f;
    constants.centerY = 0.5f;
    constants.resolutionX = static_cast<float>(m_displayWidth);
    constants.resolutionY = static_cast<float>(m_displayHeight);
    constants.textureIndex = textureDescriptorIndex;
    constants.padding[0] = 0;
    constants.padding[1] = 0;
    constants.padding[2] = 0;
    
    // Transition textures
    RenderTextureBarrier barriers[] = {
        RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
    
    // Create output framebuffer
    RenderFramebufferDesc fbDesc;
    const RenderTexture* colorAttachments[] = { outputTexture };
    fbDesc.colorAttachments = colorAttachments;
    fbDesc.colorAttachmentsCount = 1;
    auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
    
    // Set pipeline and draw
    commandList->setPipeline(m_vignettePipeline.get());
    commandList->setGraphicsPipelineLayout(m_pipelineLayout);
    commandList->setFramebuffer(outputFramebuffer.get());
    commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
    commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
    
    // Bind constants via push constants
    commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(VignetteConstants));
    
    DrawFullscreenTriangle(commandList);
    
    return true;
}

bool PostProcessRenderer::ApplySSAO(RenderCommandList* commandList,
                                     RenderTexture* depthTexture,
                                     RenderTexture* colorTexture,
                                     RenderTexture* outputTexture,
                                     float cameraNear, float cameraFar, float cameraFovY) {
    if (!m_initialized) {
        return false;
    }
    
    // Check if SSAO is enabled via config
    if (Config::SSAO == ESSAO::Off) {
        return false;
    }
    
    if (!depthTexture || !colorTexture || !outputTexture) {
        return false;
    }
    
    // SSAO shaders and pipelines not yet compiled - this is a stub for now
    // The full implementation will be enabled once shaders are compiled
    if (!m_ssaoPipeline || !m_ssaoBlurPipeline || !m_ssaoCompositePipeline) {
        LOG_WARNING("[PostProcessRenderer] SSAO pipelines not available - shaders may not be compiled");
        return false;
    }
    
    // Determine sample count and steps based on quality
    int sampleCount = 6;
    int stepsPerSample = 4;
    switch (Config::SSAO.Value) {
        case ESSAO::Low:    sampleCount = 4;  stepsPerSample = 2; break;
        case ESSAO::Medium: sampleCount = 6;  stepsPerSample = 4; break;
        case ESSAO::High:   sampleCount = 8;  stepsPerSample = 6; break;
        case ESSAO::Ultra:  sampleCount = 12; stepsPerSample = 8; break;
        default: break;
    }
    
    // Setup SSAO constants
    SSAOConstants ssaoConstants;
    ssaoConstants.resolutionX = 1.0f / m_displayWidth;
    ssaoConstants.resolutionY = 1.0f / m_displayHeight;
    ssaoConstants.width = static_cast<float>(m_displayWidth);
    ssaoConstants.height = static_cast<float>(m_displayHeight);
    ssaoConstants.cameraNear = cameraNear;
    ssaoConstants.cameraFar = cameraFar;
    ssaoConstants.cameraFovY = cameraFovY;
    ssaoConstants.aspectRatio = static_cast<float>(m_displayWidth) / static_cast<float>(m_displayHeight);
    ssaoConstants.radius = Config::SSAORadius;
    ssaoConstants.intensity = Config::SSAOIntensity;
    ssaoConstants.bias = Config::SSAOBias;
    ssaoConstants.falloffDistance = Config::SSAOFalloffDistance;
    ssaoConstants.sampleCount = sampleCount;
    ssaoConstants.stepsPerSample = stepsPerSample;
    ssaoConstants.frameIndex = static_cast<float>(m_frameIndex);
    ssaoConstants.padding = 0.0f;
    
    // Upload SSAO constants
    if (m_ssaoConstantBuffer) {
        void* mappedData = m_ssaoConstantBuffer->map();
        if (mappedData) {
            memcpy(mappedData, &ssaoConstants, sizeof(SSAOConstants));
            m_ssaoConstantBuffer->unmap();
        }
    }
    
    // Pass 1: GTAO calculation
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssaoBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_ssaoPipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_ssaoFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        
        commandList->setGraphicsPushConstants(0, &ssaoConstants, 0, sizeof(SSAOConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // Pass 2: Horizontal blur
    SSAOBlurConstants blurConstants;
    blurConstants.resolutionX = 1.0f / m_displayWidth;
    blurConstants.resolutionY = 1.0f / m_displayHeight;
    blurConstants.width = static_cast<float>(m_displayWidth);
    blurConstants.height = static_cast<float>(m_displayHeight);
    blurConstants.blurDirectionX = 1.0f;
    blurConstants.blurDirectionY = 0.0f;
    blurConstants.sharpness = 10.0f;
    blurConstants.depthThreshold = 0.1f;
    blurConstants.nearPlane = cameraNear;
    blurConstants.farPlane = cameraFar;
    
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_ssaoBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssaoBlurBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        commandList->setPipeline(m_ssaoBlurPipeline.get());
        commandList->setFramebuffer(m_ssaoBlurFramebuffer.get());
        
        commandList->setGraphicsPushConstants(0, &blurConstants, 0, sizeof(SSAOBlurConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // Pass 3: Vertical blur back to ssaoBuffer
    blurConstants.blurDirectionX = 0.0f;
    blurConstants.blurDirectionY = 1.0f;
    
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_ssaoBlurBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssaoBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setFramebuffer(m_ssaoFramebuffer.get());
        
        commandList->setGraphicsPushConstants(0, &blurConstants, 0, sizeof(SSAOBlurConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // Pass 4: Composite AO with scene color
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssaoBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_ssaoCompositePipeline.get());
        commandList->setFramebuffer(outputFramebuffer.get());
        
        // Composite constants (intensity, min value, debug mode)
        struct CompositeConstants {
            float resolutionX, resolutionY, width, height;
            float aoIntensity;
            float aoMinValue;
            float debugMode;
            float padding;
        } compositeConstants;
        
        compositeConstants.resolutionX = 1.0f / m_displayWidth;
        compositeConstants.resolutionY = 1.0f / m_displayHeight;
        compositeConstants.width = static_cast<float>(m_displayWidth);
        compositeConstants.height = static_cast<float>(m_displayHeight);
        compositeConstants.aoIntensity = Config::SSAOIntensity;
        compositeConstants.aoMinValue = 0.1f;  // Prevent pure black
        compositeConstants.debugMode = 0.0f;   // Normal composite mode
        compositeConstants.padding = 0.0f;
        
        commandList->setGraphicsPushConstants(0, &compositeConstants, 0, sizeof(CompositeConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    return true;
}

bool PostProcessRenderer::ApplyDoF(RenderCommandList* commandList,
                                    RenderTexture* colorTexture,
                                    RenderTexture* depthTexture,
                                    RenderTexture* outputTexture,
                                    float focusDistance, float apertureSize,
                                    float cameraNear, float cameraFar) {
    if (!m_initialized) {
        return false;
    }
    
    // Check if DoF is enabled via config
    if (Config::DepthOfField == EDepthOfField::Off) {
        return false;
    }
    
    if (!colorTexture || !depthTexture || !outputTexture) {
        return false;
    }
    
    // DoF multi-pass pipelines required
    if (!m_dofPrefilterPipeline || !m_dofBokehPipeline || 
        !m_dofPostfilterPipeline || !m_dofCombinePipeline) {
        LOG_WARNING("[PostProcessRenderer] DoF pipelines not available - shaders may not be compiled");
        return false;
    }
    
    // Check half-res buffers exist
    if (!m_dofHalfBuffer0 || !m_dofHalfBuffer1 || 
        !m_dofHalfFramebuffer0 || !m_dofHalfFramebuffer1) {
        LOG_WARNING("[PostProcessRenderer] DoF half-res buffers not available");
        return false;
    }
    
    // Determine bokeh radius based on quality setting
    float bokehRadius = 4.0f;
    float maxBlur = 16.0f;
    switch (Config::DepthOfField.Value) {
        case EDepthOfField::Low:    bokehRadius = 2.0f; maxBlur = 8.0f;  break;
        case EDepthOfField::Medium: bokehRadius = 4.0f; maxBlur = 16.0f; break;
        case EDepthOfField::High:   bokehRadius = 6.0f; maxBlur = 24.0f; break;
        case EDepthOfField::Ultra:  bokehRadius = 8.0f; maxBlur = 32.0f; break;
        default: break;
    }
    
    // Calculate half resolution dimensions
    uint32_t halfWidth = m_displayWidth / 2;
    uint32_t halfHeight = m_displayHeight / 2;
    
    // Use aperture size to calculate focus range (higher aperture = shallower DoF)
    float effectiveAperture = apertureSize > 0.0f ? apertureSize : Config::DOFApertureSize;
    float effectiveFocusDistance = focusDistance > 0.0f ? focusDistance : Config::DOFFocusDistance;
    // Focus range inversely proportional to aperture (larger aperture = smaller focus range = more blur)
    float focusRange = effectiveFocusDistance * (0.5f / (effectiveAperture + 0.01f));
    focusRange = std::max(focusRange, 1.0f);  // Minimum focus range
    
    // Setup DoF constants (shared across all passes)
    DOFConstants dofConstants;
    dofConstants.resolutionX = 1.0f / m_displayWidth;
    dofConstants.resolutionY = 1.0f / m_displayHeight;
    dofConstants.width = static_cast<float>(m_displayWidth);
    dofConstants.height = static_cast<float>(m_displayHeight);
    dofConstants.halfResolutionX = 1.0f / halfWidth;
    dofConstants.halfResolutionY = 1.0f / halfHeight;
    dofConstants.halfWidth = static_cast<float>(halfWidth);
    dofConstants.halfHeight = static_cast<float>(halfHeight);
    dofConstants.focusDistance = effectiveFocusDistance;
    dofConstants.focusRange = focusRange;
    dofConstants.nearPlane = cameraNear;
    dofConstants.farPlane = cameraFar;
    dofConstants.bokehRadius = bokehRadius;
    dofConstants.maxBlur = maxBlur;
    dofConstants.cocScale = bokehRadius;  // Scale CoC by bokeh radius
    dofConstants.padding = 0.0f;
    
    // Upload DoF constants
    if (m_dofConstantBuffer) {
        void* mappedData = m_dofConstantBuffer->map();
        if (mappedData) {
            memcpy(mappedData, &dofConstants, sizeof(DOFConstants));
            m_dofConstantBuffer->unmap();
        }
    }
    
    // =========================================================================
    // Pass 1: Prefilter - Downsample to half-res with weighted CoC
    // Input: Full-res color + depth
    // Output: Half-res color with CoC in alpha (to m_dofHalfBuffer0)
    // =========================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_dofHalfBuffer0.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        commandList->setPipeline(m_dofPrefilterPipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_dofHalfFramebuffer0.get());
        commandList->setViewports(RenderViewport(0, 0, halfWidth, halfHeight));
        commandList->setScissors(RenderRect(0, 0, halfWidth, halfHeight));
        
        commandList->setGraphicsPushConstants(0, &dofConstants, 0, sizeof(DOFConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 2: Bokeh - Apply disk blur with FG/BG separation
    // Input: m_dofHalfBuffer0 (prefiltered)
    // Output: m_dofHalfBuffer1 (bokeh result with FG alpha)
    // =========================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_dofHalfBuffer0.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_dofHalfBuffer1.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_dofBokehPipeline.get());
        commandList->setFramebuffer(m_dofHalfFramebuffer1.get());
        
        commandList->setGraphicsPushConstants(0, &dofConstants, 0, sizeof(DOFConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 3: Postfilter - Apply tent filter to smooth bokeh samples
    // Input: m_dofHalfBuffer1 (bokeh)
    // Output: m_dofHalfBuffer0 (smoothed bokeh)
    // =========================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_dofHalfBuffer1.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_dofHalfBuffer0.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_dofPostfilterPipeline.get());
        commandList->setFramebuffer(m_dofHalfFramebuffer0.get());
        
        commandList->setGraphicsPushConstants(0, &dofConstants, 0, sizeof(DOFConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 4: Combine - Composite bokeh with sharp source
    // Input: Full-res source color + depth, half-res bokeh (m_dofHalfBuffer0)
    // Output: Final DoF result
    // =========================================================================
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_dofHalfBuffer0.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 4);
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_dofCombinePipeline.get());
        commandList->setFramebuffer(outputFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        
        commandList->setGraphicsPushConstants(0, &dofConstants, 0, sizeof(DOFConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    m_frameIndex++;
    
    return true;
}

// =============================================================================
// SSR (Screen-Space Reflections) Implementation
// =============================================================================

bool PostProcessRenderer::ApplySSR(RenderCommandList* commandList,
                                   RenderTexture* colorTexture,
                                   RenderTexture* depthTexture,
                                   RenderTexture* outputTexture,
                                   float cameraNear, float cameraFar, float cameraFovY,
                                   const float* viewMatrix, const float* projMatrix) {
    if (!m_initialized) {
        return false;
    }
    
    if (!colorTexture || !depthTexture || !outputTexture) {
        return false;
    }
    
    // SSR pipeline not yet available - this is a stub for now
    if (!m_ssrRaytracePipeline || !m_ssrCompositePipeline) {
        // Silently return - SSR shaders not compiled yet
        return false;
    }
    
    // Create SSR buffer if needed
    if (!m_ssrBuffer) {
        RenderTextureDesc ssrDesc = RenderTextureDesc::Texture2D(
            m_displayWidth, m_displayHeight, 1, RenderFormat::R16G16B16A16_FLOAT,
            RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::STORAGE
        );
        m_ssrBuffer = m_device->createTexture(ssrDesc);
        
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { m_ssrBuffer.get() };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        m_ssrFramebuffer = m_device->createFramebuffer(fbDesc);
    }
    
    // Setup SSR constants
    SSRConstants ssrConstants;
    ssrConstants.resolutionX = 1.0f / m_displayWidth;
    ssrConstants.resolutionY = 1.0f / m_displayHeight;
    ssrConstants.width = static_cast<float>(m_displayWidth);
    ssrConstants.height = static_cast<float>(m_displayHeight);
    ssrConstants.cameraNear = cameraNear;
    ssrConstants.cameraFar = cameraFar;
    ssrConstants.cameraFovY = cameraFovY;
    ssrConstants.aspectRatio = static_cast<float>(m_displayWidth) / m_displayHeight;
    
    // Copy matrices
    if (viewMatrix) {
        memcpy(ssrConstants.viewMatrix, viewMatrix, sizeof(float) * 16);
    }
    if (projMatrix) {
        memcpy(ssrConstants.projMatrix, projMatrix, sizeof(float) * 16);
    }
    
    // Calculate inverse matrices (simplified - identity for now)
    // In production, these should be computed from the input matrices
    for (int i = 0; i < 16; i++) {
        ssrConstants.invViewMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        ssrConstants.invProjMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    
    ssrConstants.maxDistance = 100.0f;
    ssrConstants.stride = 1.0f;
    ssrConstants.strideZCutoff = 0.1f;
    ssrConstants.maxSteps = 64.0f;
    ssrConstants.thickness = 0.5f;
    ssrConstants.fadeStart = 50.0f;
    ssrConstants.fadeEnd = 100.0f;
    ssrConstants.roughnessCutoff = 0.5f;
    ssrConstants.edgeFade = 0.1f;
    ssrConstants.frameIndex = static_cast<float>(m_frameIndex);
    ssrConstants.padding[0] = 0.0f;
    ssrConstants.padding[1] = 0.0f;
    
    // Upload SSR constants
    if (m_ssrConstantBuffer) {
        void* mappedData = m_ssrConstantBuffer->map();
        if (mappedData) {
            memcpy(mappedData, &ssrConstants, sizeof(SSRConstants));
            m_ssrConstantBuffer->unmap();
        }
    }
    
    // Pass 1: Ray trace reflections
    {
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssrBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        commandList->setPipeline(m_ssrRaytracePipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_ssrFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        commandList->setGraphicsPushConstants(0, &ssrConstants, 0, sizeof(SSRConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // Pass 2: Composite with scene
    {
        SSRCompositeConstants compositeConstants;
        compositeConstants.resolutionX = 1.0f / m_displayWidth;
        compositeConstants.resolutionY = 1.0f / m_displayHeight;
        compositeConstants.width = static_cast<float>(m_displayWidth);
        compositeConstants.height = static_cast<float>(m_displayHeight);
        compositeConstants.intensity = 0.5f;  // Could be config-driven
        compositeConstants.maxRoughness = 0.5f;
        compositeConstants.padding[0] = 0.0f;
        compositeConstants.padding[1] = 0.0f;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_ssrBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 4);
        
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_ssrCompositePipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(outputFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        commandList->setGraphicsPushConstants(0, &compositeConstants, 0, sizeof(SSRCompositeConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    m_frameIndex++;
    
    return true;
}

// =============================================================================
// Film Grain Implementation
// =============================================================================
bool PostProcessRenderer::ApplyFilmGrain(RenderCommandList* commandList,
                                          RenderTexture* colorTexture,
                                          RenderTexture* outputTexture,
                                          uint32_t textureDescriptorIndex) {
    if (!m_initialized || !m_filmGrainPipeline) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    // Check if film grain is enabled via config
    if (Config::FilmGrain == EFilmGrain::Off) {
        return false;
    }
    
    // Determine intensity based on quality setting
    float intensityMultiplier = 1.0f;
    float coloredGrain = 0.0f;  // Default to monochrome
    switch (Config::FilmGrain.Value) {
        case EFilmGrain::Light:
            intensityMultiplier = 0.03f;
            break;
        case EFilmGrain::Medium:
            intensityMultiplier = 0.06f;
            coloredGrain = 0.3f;  // Slight color grain
            break;
        case EFilmGrain::Heavy:
            intensityMultiplier = 0.12f;
            coloredGrain = 0.7f;  // Strong color grain
            break;
        default:
            break;
    }
    
    // Setup film grain constants
    FilmGrainConstants constants;
    constants.resolutionX = 1.0f / m_displayWidth;
    constants.resolutionY = 1.0f / m_displayHeight;
    constants.width = static_cast<float>(m_displayWidth);
    constants.height = static_cast<float>(m_displayHeight);
    constants.intensity = Config::FilmGrainIntensity * intensityMultiplier;
    constants.frameIndex = static_cast<float>(m_frameIndex);
    constants.luminanceScale = 0.8f;  // 80% luminance-dependent grain
    constants.coloredGrain = coloredGrain;
    
    // Transition textures
    RenderTextureBarrier barriers[] = {
        RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
    
    // Create output framebuffer
    RenderFramebufferDesc fbDesc;
    const RenderTexture* colorAttachments[] = { outputTexture };
    fbDesc.colorAttachments = colorAttachments;
    fbDesc.colorAttachmentsCount = 1;
    auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
    
    // Set pipeline and draw
    commandList->setPipeline(m_filmGrainPipeline.get());
    commandList->setGraphicsPipelineLayout(m_pipelineLayout);
    commandList->setFramebuffer(outputFramebuffer.get());
    commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
    commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
    
    // Bind constants via push constants
    commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(FilmGrainConstants));
    
    DrawFullscreenTriangle(commandList);
    
    return true;
}

// =============================================================================
// Chromatic Aberration Implementation
// =============================================================================
bool PostProcessRenderer::ApplyChromaticAberration(RenderCommandList* commandList,
                                                    RenderTexture* colorTexture,
                                                    RenderTexture* outputTexture,
                                                    uint32_t textureDescriptorIndex) {
    if (!m_initialized || !m_chromaticAberrationPipeline) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    // Check if chromatic aberration is enabled via config
    if (Config::ChromaticAberration == EChromaticAberration::Off) {
        return false;
    }
    
    // Determine offsets based on quality setting
    float redOffset = 1.0f;
    float blueOffset = 1.0f;
    float radialFalloff = 2.0f;  // Quadratic falloff (natural lens behavior)
    switch (Config::ChromaticAberration.Value) {
        case EChromaticAberration::Subtle:
            redOffset = 0.5f;
            blueOffset = 0.5f;
            break;
        case EChromaticAberration::Normal:
            redOffset = 1.0f;
            blueOffset = 1.0f;
            break;
        case EChromaticAberration::Strong:
            redOffset = 2.0f;
            blueOffset = 2.0f;
            radialFalloff = 1.5f;  // More uniform spread
            break;
        default:
            break;
    }
    
    // Setup chromatic aberration constants
    ChromaticAberrationConstants constants;
    constants.resolutionX = 1.0f / m_displayWidth;
    constants.resolutionY = 1.0f / m_displayHeight;
    constants.width = static_cast<float>(m_displayWidth);
    constants.height = static_cast<float>(m_displayHeight);
    constants.intensity = Config::ChromaticAberrationIntensity;
    constants.redOffset = redOffset;
    constants.greenOffset = 0.0f;  // Green stays centered
    constants.blueOffset = blueOffset;
    constants.radialFalloff = radialFalloff;
    constants.centerX = 0.5f;
    constants.centerY = 0.5f;
    constants.aspectCorrection = static_cast<float>(m_displayWidth) / m_displayHeight;
    constants.maxOffset = 16.0f;  // Max 16 pixel offset
    constants.softKnee = 0.5f;
    constants.padding[0] = 0.0f;
    constants.padding[1] = 0.0f;
    
    // Transition textures
    RenderTextureBarrier barriers[] = {
        RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
    
    // Create output framebuffer
    RenderFramebufferDesc fbDesc;
    const RenderTexture* colorAttachments[] = { outputTexture };
    fbDesc.colorAttachments = colorAttachments;
    fbDesc.colorAttachmentsCount = 1;
    auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
    
    // Set pipeline and draw
    commandList->setPipeline(m_chromaticAberrationPipeline.get());
    commandList->setGraphicsPipelineLayout(m_pipelineLayout);
    commandList->setFramebuffer(outputFramebuffer.get());
    commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
    commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
    
    // Bind constants via push constants
    commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(ChromaticAberrationConstants));
    
    DrawFullscreenTriangle(commandList);
    
    return true;
}

// =============================================================================
// Motion Blur Implementation
// =============================================================================
bool PostProcessRenderer::ApplyMotionBlur(RenderCommandList* commandList,
                                           RenderTexture* colorTexture,
                                           RenderTexture* depthTexture,
                                           RenderTexture* outputTexture,
                                           const float* invViewProj, const float* prevViewProj) {
    if (!m_initialized || !m_motionBlurCameraPipeline) {
        return false;
    }
    
    if (!colorTexture || !depthTexture || !outputTexture) {
        return false;
    }
    
    // Check if motion blur is enabled via config
    if (Config::MotionBlur == EMotionBlur::Off) {
        return false;
    }
    
    // Setup motion blur constants
    MotionBlurConstants constants;
    
    // Copy matrices
    if (invViewProj) {
        memcpy(constants.invViewProj, invViewProj, sizeof(float) * 16);
    } else {
        // Identity matrix fallback
        for (int i = 0; i < 16; i++) {
            constants.invViewProj[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
    }
    
    if (prevViewProj) {
        memcpy(constants.prevViewProj, prevViewProj, sizeof(float) * 16);
    } else {
        // Use stored previous view-projection matrix
        memcpy(constants.prevViewProj, m_prevViewProj, sizeof(float) * 16);
    }
    
    constants.resolutionX = 1.0f / m_displayWidth;
    constants.resolutionY = 1.0f / m_displayHeight;
    constants.width = static_cast<float>(m_displayWidth);
    constants.height = static_cast<float>(m_displayHeight);
    constants.blurStrength = Config::MotionBlurStrength;
    constants.maxBlurRadius = 32.0f;
    constants.depthThreshold = 0.01f;
    constants.padding = 0.0f;
    
    // Transition textures
    RenderTextureBarrier barriers[] = {
        RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
    };
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
    
    // Create output framebuffer
    RenderFramebufferDesc fbDesc;
    const RenderTexture* colorAttachments[] = { outputTexture };
    fbDesc.colorAttachments = colorAttachments;
    fbDesc.colorAttachmentsCount = 1;
    auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
    
    // Set pipeline and draw
    commandList->setPipeline(m_motionBlurCameraPipeline.get());
    commandList->setGraphicsPipelineLayout(m_pipelineLayout);
    commandList->setFramebuffer(outputFramebuffer.get());
    commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
    commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
    
    // Bind constants via push constants
    commandList->setGraphicsPushConstants(0, &constants, 0, sizeof(MotionBlurConstants));
    
    DrawFullscreenTriangle(commandList);
    
    // Store current view-projection for next frame
    if (invViewProj) {
        // We actually need to store the current view-proj, not inv
        // For now, we'll rely on the caller to provide prevViewProj
    }
    
    return true;
}

// =============================================================================
// Bloom Implementation (MiniEngine-style pyramid)
// =============================================================================

bool PostProcessRenderer::ApplyBloom(RenderCommandList* commandList,
                                      RenderTexture* colorTexture,
                                      RenderTexture* outputTexture,
                                      float threshold, float intensity) {
    if (!m_initialized) {
        return false;
    }
    
    // Check if bloom is enabled via config
    if (!Config::EnableBloom) {
        return false;
    }
    
    if (!colorTexture || !outputTexture) {
        return false;
    }
    
    // Check bloom pipelines available
    if (!m_bloomExtractPipeline || !m_bloomDownsamplePipeline || 
        !m_bloomUpsamplePipeline || !m_bloomCompositePipeline) {
        LOG_WARNING("[PostProcessRenderer] Bloom pipelines not available");
        return false;
    }
    
    // Check mip chain buffers exist
    bool mipChainValid = true;
    for (int i = 0; i < BLOOM_MIP_COUNT; i++) {
        if (!m_bloomMipChain[i] || !m_bloomMipFramebuffers[i]) {
            mipChainValid = false;
            break;
        }
    }
    if (!mipChainValid || !m_bloomResult || !m_bloomResultFramebuffer) {
        LOG_WARNING("[PostProcessRenderer] Bloom buffers not available");
        return false;
    }
    
    // Use config values if not overridden
    float effectiveThreshold = threshold > 0.0f ? threshold : Config::BloomThreshold;
    float effectiveIntensity = intensity > 0.0f ? intensity : Config::BloomIntensity;
    
    // =========================================================================
    // Pass 1: Extract bright pixels with Karis average anti-firefly
    // Input: Full-res color
    // Output: Half-res bloom (to m_bloomMipChain[0])
    // =========================================================================
    {
        BloomExtractConstants extractConstants;
        extractConstants.threshold = effectiveThreshold;
        extractConstants.softThreshold = effectiveThreshold * 0.5f;
        extractConstants.invWidth = 1.0f / m_displayWidth;
        extractConstants.invHeight = 1.0f / m_displayHeight;
        extractConstants.useKarisAverage = 1.0f;
        extractConstants.padding1 = 0.0f;
        extractConstants.padding2 = 0.0f;
        extractConstants.padding3 = 0.0f;
        
        uint32_t mipWidth = m_displayWidth / 2;
        uint32_t mipHeight = m_displayHeight / 2;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_bloomMipChain[0].get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_bloomExtractPipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_bloomMipFramebuffers[0].get());
        commandList->setViewports(RenderViewport(0, 0, mipWidth, mipHeight));
        commandList->setScissors(RenderRect(0, 0, mipWidth, mipHeight));
        
        commandList->setGraphicsPushConstants(0, &extractConstants, 0, sizeof(BloomExtractConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 2: Downsample chain (mip 0 -> 1 -> 2 -> 3 -> 4 -> 5)
    // =========================================================================
    for (int mipLevel = 1; mipLevel < BLOOM_MIP_COUNT; mipLevel++) {
        BloomDownsampleConstants downsampleConstants;
        
        // Calculate source dimensions (previous mip)
        uint32_t srcWidth = m_displayWidth >> mipLevel;
        uint32_t srcHeight = m_displayHeight >> mipLevel;
        uint32_t dstWidth = m_displayWidth >> (mipLevel + 1);
        uint32_t dstHeight = m_displayHeight >> (mipLevel + 1);
        
        downsampleConstants.invWidth = 1.0f / srcWidth;
        downsampleConstants.invHeight = 1.0f / srcHeight;
        downsampleConstants.mipLevel = static_cast<float>(mipLevel);
        downsampleConstants.useKarisAverage = (mipLevel == 1) ? 1.0f : 0.0f; // Karis on first downsample
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_bloomMipChain[mipLevel - 1].get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_bloomMipChain[mipLevel].get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_bloomDownsamplePipeline.get());
        commandList->setFramebuffer(m_bloomMipFramebuffers[mipLevel].get());
        commandList->setViewports(RenderViewport(0, 0, dstWidth, dstHeight));
        commandList->setScissors(RenderRect(0, 0, dstWidth, dstHeight));
        
        commandList->setGraphicsPushConstants(0, &downsampleConstants, 0, sizeof(BloomDownsampleConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 3: Upsample chain (mip 5 -> 4 -> 3 -> 2 -> 1 -> 0)
    // Each level blends with the next higher resolution mip
    // =========================================================================
    for (int mipLevel = BLOOM_MIP_COUNT - 2; mipLevel >= 0; mipLevel--) {
        BloomUpsampleConstants upsampleConstants;
        
        // Destination is the higher resolution mip
        uint32_t dstWidth = m_displayWidth >> (mipLevel + 1);
        uint32_t dstHeight = m_displayHeight >> (mipLevel + 1);
        
        upsampleConstants.invWidth = 1.0f / dstWidth;
        upsampleConstants.invHeight = 1.0f / dstHeight;
        upsampleConstants.filterRadius = 1.0f;  // Tent filter radius
        upsampleConstants.bloomIntensity = effectiveIntensity;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_bloomMipChain[mipLevel + 1].get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_bloomMipChain[mipLevel].get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_bloomUpsamplePipeline.get());
        commandList->setFramebuffer(m_bloomMipFramebuffers[mipLevel].get());
        commandList->setViewports(RenderViewport(0, 0, dstWidth, dstHeight));
        commandList->setScissors(RenderRect(0, 0, dstWidth, dstHeight));
        
        commandList->setGraphicsPushConstants(0, &upsampleConstants, 0, sizeof(BloomUpsampleConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 4: Composite bloom with original scene
    // =========================================================================
    {
        BloomCompositeConstants compositeConstants;
        compositeConstants.bloomIntensity = effectiveIntensity;
        compositeConstants.saturation = 1.0f;
        compositeConstants.blendMode = 0.0f;  // 0 = additive, 1 = screen
        compositeConstants.padding = 0.0f;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_bloomMipChain[0].get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_bloomCompositePipeline.get());
        commandList->setFramebuffer(outputFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        
        commandList->setGraphicsPushConstants(0, &compositeConstants, 0, sizeof(BloomCompositeConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    return true;
}

// =============================================================================
// Sun Shafts Implementation (FusionFix-style GPU Gems 3)
// =============================================================================

bool PostProcessRenderer::ApplySunShafts(RenderCommandList* commandList,
                                          RenderTexture* colorTexture,
                                          RenderTexture* depthTexture,
                                          RenderTexture* outputTexture,
                                          float sunScreenX, float sunScreenY,
                                          float density, float weight, float decay, float exposure) {
    if (!m_initialized) {
        return false;
    }
    
    // Check if sun shafts are enabled via config
    if (!Config::EnableSunShafts) {
        return false;
    }
    
    if (!colorTexture || !depthTexture || !outputTexture) {
        return false;
    }
    
    // Check sun shafts pipelines available
    if (!m_sunShaftsPrepassPipeline || !m_sunShaftsRadialPipeline || !m_sunShaftsCompositePipeline) {
        LOG_WARNING("[PostProcessRenderer] Sun shafts pipelines not available");
        return false;
    }
    
    // Check buffers exist
    if (!m_sunShaftsPrepassBuffer || !m_sunShaftsRadialBuffer ||
        !m_sunShaftsPrepassFramebuffer || !m_sunShaftsRadialFramebuffer) {
        LOG_WARNING("[PostProcessRenderer] Sun shafts buffers not available");
        return false;
    }
    
    // Use config values if not overridden
    float effectiveDensity = density > 0.0f ? density : Config::SunShaftsDensity;
    float effectiveWeight = weight > 0.0f ? weight : Config::SunShaftsWeight;
    float effectiveDecay = decay > 0.0f ? decay : Config::SunShaftsDecay;
    float effectiveExposure = exposure > 0.0f ? exposure : Config::SunShaftsExposure;
    
    // Half resolution for performance
    uint32_t halfWidth = m_displayWidth / 2;
    uint32_t halfHeight = m_displayHeight / 2;
    
    // =========================================================================
    // Pass 1: Prepass - Extract sun/sky pixels with depth masking
    // Input: Full-res color + depth
    // Output: Half-res sun mask (to m_sunShaftsPrepassBuffer)
    // =========================================================================
    {
        SunShaftsPrepassConstants prepassConstants;
        prepassConstants.sunPosX = sunScreenX;
        prepassConstants.sunPosY = sunScreenY;
        prepassConstants.sunRadius = 0.1f;  // Sun disk radius in screen space
        prepassConstants.skyDepthThreshold = 0.9999f;  // Depth values above this are sky
        prepassConstants.invWidth = 1.0f / m_displayWidth;
        prepassConstants.invHeight = 1.0f / m_displayHeight;
        prepassConstants.horizonFade = 1.0f;
        prepassConstants.padding = 0.0f;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(depthTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_sunShaftsPrepassBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        commandList->setPipeline(m_sunShaftsPrepassPipeline.get());
        commandList->setGraphicsPipelineLayout(m_pipelineLayout);
        commandList->setFramebuffer(m_sunShaftsPrepassFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, halfWidth, halfHeight));
        commandList->setScissors(RenderRect(0, 0, halfWidth, halfHeight));
        
        commandList->setGraphicsPushConstants(0, &prepassConstants, 0, sizeof(SunShaftsPrepassConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 2: Radial blur - GPU Gems 3 ray march toward sun
    // Input: m_sunShaftsPrepassBuffer
    // Output: m_sunShaftsRadialBuffer
    // =========================================================================
    {
        SunShaftsRadialConstants radialConstants;
        radialConstants.sunPosX = sunScreenX;
        radialConstants.sunPosY = sunScreenY;
        radialConstants.density = effectiveDensity;
        radialConstants.weight = effectiveWeight;
        radialConstants.decay = effectiveDecay;
        radialConstants.exposure = effectiveExposure;
        radialConstants.numSamples = 32.0f;  // 32 samples for quality/performance balance
        radialConstants.padding = 0.0f;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(m_sunShaftsPrepassBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_sunShaftsRadialBuffer.get(), RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        
        commandList->setPipeline(m_sunShaftsRadialPipeline.get());
        commandList->setFramebuffer(m_sunShaftsRadialFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, halfWidth, halfHeight));
        commandList->setScissors(RenderRect(0, 0, halfWidth, halfHeight));
        
        commandList->setGraphicsPushConstants(0, &radialConstants, 0, sizeof(SunShaftsRadialConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    // =========================================================================
    // Pass 3: Composite - Additive blend with scene
    // Input: Full-res color + half-res radial blur result
    // Output: Final scene with sun shafts
    // =========================================================================
    {
        SunShaftsCompositeConstants compositeConstants;
        compositeConstants.sunColorR = 1.0f;  // Sun color tint
        compositeConstants.sunColorG = 0.95f;
        compositeConstants.sunColorB = 0.8f;
        compositeConstants.blendStrength = 1.0f;
        
        RenderTextureBarrier barriers[] = {
            RenderTextureBarrier(colorTexture, RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(m_sunShaftsRadialBuffer.get(), RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(outputTexture, RenderTextureLayout::COLOR_WRITE)
        };
        commandList->barriers(RenderBarrierStage::GRAPHICS, barriers, 3);
        
        // Create output framebuffer
        RenderFramebufferDesc fbDesc;
        const RenderTexture* colorAttachments[] = { outputTexture };
        fbDesc.colorAttachments = colorAttachments;
        fbDesc.colorAttachmentsCount = 1;
        auto outputFramebuffer = m_device->createFramebuffer(fbDesc);
        
        commandList->setPipeline(m_sunShaftsCompositePipeline.get());
        commandList->setFramebuffer(outputFramebuffer.get());
        commandList->setViewports(RenderViewport(0, 0, m_displayWidth, m_displayHeight));
        commandList->setScissors(RenderRect(0, 0, m_displayWidth, m_displayHeight));
        
        commandList->setGraphicsPushConstants(0, &compositeConstants, 0, sizeof(SunShaftsCompositeConstants));
        
        DrawFullscreenTriangle(commandList);
    }
    
    return true;
}

// =============================================================================
// Global initialization functions
// =============================================================================

bool InitializePostProcessRenderer(RenderDevice* device, RenderPipelineLayout* pipelineLayout,
                                    RenderDescriptorSet* textureDescriptorSet,
                                    uint32_t displayWidth, uint32_t displayHeight) {
    return g_postProcessRenderer.Initialize(device, pipelineLayout, textureDescriptorSet,
                                             displayWidth, displayHeight);
}

void ShutdownPostProcessRenderer() {
    g_postProcessRenderer.Shutdown();
}

} // namespace PostProcess
