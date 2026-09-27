// =============================================================================
// Post-Process Renderer - Actual Implementation
// TAA, SMAA, FSR 1.0 rendering pipelines with proper resource management
// =============================================================================
#pragma once

#include <plume_render_interface.h>
#include <memory>
#include <cstdint>

using namespace plume;

namespace PostProcess {

// Forward declarations
struct PostProcessResources;

// =============================================================================
// TAA Constants - matches taa_ps.hlsl cbuffer
// =============================================================================
struct TAAConstants {
    float resolutionX;      // 1/width
    float resolutionY;      // 1/height  
    float width;
    float height;
    float jitterX;
    float jitterY;
    float prevJitterX;
    float prevJitterY;
    float blendFactor;      // 0.05 - 0.1 typical
    float motionScale;
    float padding[2];
};

// =============================================================================
// SMAA Constants - matches smaa_edge_detect_ps.hlsl cbuffer
// =============================================================================
struct SMAAConstants {
    float resolutionX;      // 1/width
    float resolutionY;      // 1/height
    float width;
    float height;
};

// =============================================================================
// FSR 1.0 Constants - matches fsr1_easu_ps.hlsl cbuffer
// =============================================================================
struct FSR1Constants {
    float inputWidth;
    float inputHeight;
    float inputRcpWidth;
    float inputRcpHeight;
    float outputWidth;
    float outputHeight;
    float outputRcpWidth;
    float outputRcpHeight;
    float scaleX;           // inputWidth/outputWidth
    float scaleY;           // inputHeight/outputHeight
    float halfScaleX;
    float halfScaleY;
    float reserved[4];
};

// =============================================================================
// Vignette Constants - matches vignette_ps.hlsl cbuffer
// =============================================================================
struct VignetteConstants {
    float intensity;      // Overall vignette strength (0 = off, 1 = full)
    float radius;         // Inner radius where vignette starts (0-1)
    float softness;       // Falloff softness (higher = softer edge)
    float roundness;      // Aspect ratio correction (1 = circular)
    float centerX;        // Vignette center X (usually 0.5)
    float centerY;        // Vignette center Y (usually 0.5)
    float resolutionX;    // Screen width
    float resolutionY;    // Screen height
    uint32_t textureIndex;// Input texture descriptor index
    uint32_t padding[3];
};

// =============================================================================
// Chromatic Aberration Constants - matches chromatic_aberration_ps.hlsl cbuffer
// =============================================================================
struct ChromaticAberrationConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;              // Screen width
    float height;             // Screen height
    float intensity;          // Overall intensity multiplier
    float redOffset;          // Red channel offset multiplier (positive = outward)
    float greenOffset;        // Green channel offset multiplier (usually small/zero)
    float blueOffset;         // Blue channel offset multiplier (positive = outward)
    float radialFalloff;      // How much effect increases toward edges (1.0 = linear, 2.0 = quadratic)
    float centerX;            // Custom center point X (default 0.5)
    float centerY;            // Custom center point Y (default 0.5)
    float aspectCorrection;   // Aspect ratio correction (width/height, or 1.0 to disable)
    float maxOffset;          // Maximum offset in pixels (prevents extreme distortion)
    float softKnee;           // Falloff smoothness (0 = sharp, 1 = smooth)
    float padding[2];         // Alignment padding
};

// =============================================================================
// SSAO Constants - matches ssao_gtao_ps.hlsl cbuffer
// =============================================================================
struct SSAOConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float cameraNear;         // Camera near plane
    float cameraFar;          // Camera far plane
    float cameraFovY;         // Vertical FOV in radians
    float aspectRatio;        // Width/Height
    float projMatrix[16];     // Projection matrix (row-major)
    float invProjMatrix[16];  // Inverse projection matrix
    float radius;             // World-space AO radius
    float intensity;          // AO intensity multiplier
    float bias;               // Depth comparison bias
    float falloffDistance;    // Distance at which AO fades out
    int sampleCount;          // Number of direction samples
    int stepsPerSample;       // Steps per direction
    float frameIndex;         // For temporal jitter
    float padding;
};

// =============================================================================
// SSAO Blur Constants - matches ssao_blur_ps.hlsl cbuffer
// =============================================================================
struct SSAOBlurConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float blurDirectionX;     // 1 for horizontal, 0 for vertical
    float blurDirectionY;     // 0 for horizontal, 1 for vertical
    float sharpness;          // Edge sharpness
    float depthThreshold;     // Depth threshold for edge detection
    float nearPlane;
    float farPlane;
    float padding[2];
};

// =============================================================================
// DoF Constants - matches dof_*_ps.hlsl cbuffers (multi-pass system)
// =============================================================================
struct DOFConstants {
    // Resolution info - full res
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    // Resolution info - half res
    float halfResolutionX;    // 1/(width/2)
    float halfResolutionY;    // 1/(height/2)
    float halfWidth;
    float halfHeight;
    // DoF parameters
    float focusDistance;      // Distance to focus plane (world units)
    float focusRange;         // Range where things are in focus
    float nearPlane;          // Camera near plane
    float farPlane;           // Camera far plane
    float bokehRadius;        // Bokeh radius in half-res texels (1-10, default 4)
    float maxBlur;            // Maximum blur radius in pixels
    float cocScale;           // CoC scale factor
    float padding;
};

// =============================================================================
// SSR Constants - matches ssr_raytrace_ps.hlsl cbuffer
// =============================================================================
struct SSRConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float cameraNear;
    float cameraFar;
    float cameraFovY;
    float aspectRatio;
    float viewMatrix[16];     // View matrix (row-major)
    float projMatrix[16];     // Projection matrix
    float invViewMatrix[16];  // Inverse view matrix
    float invProjMatrix[16];  // Inverse projection matrix
    float maxDistance;        // Maximum ray distance
    float stride;             // Ray march stride
    float strideZCutoff;      // Z cutoff for stride
    float maxSteps;           // Maximum ray steps
    float thickness;          // Surface thickness
    float fadeStart;          // Fade start distance
    float fadeEnd;            // Fade end distance
    float roughnessCutoff;    // Roughness threshold
    float edgeFade;           // Screen edge fade
    float frameIndex;         // Temporal jitter
    float padding[2];
};

// =============================================================================
// SSR Composite Constants - matches ssr_composite_ps.hlsl cbuffer
// =============================================================================
struct SSRCompositeConstants {
    float resolutionX;
    float resolutionY;
    float width;
    float height;
    float intensity;          // Overall SSR intensity
    float maxRoughness;       // Max roughness for reflections
    float padding[2];
};

// =============================================================================
// Film Grain Constants - matches film_grain_ps.hlsl cbuffer
// =============================================================================
struct FilmGrainConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float intensity;          // Grain intensity (0.0-1.0)
    float frameIndex;         // Frame index for temporal animation
    float luminanceScale;     // How much grain affects bright vs dark areas
    float coloredGrain;       // 0 = monochrome, 1 = colored grain
};

// =============================================================================
// Motion Blur Constants - matches motion_blur_camera_ps.hlsl cbuffer
// =============================================================================
struct MotionBlurConstants {
    float invViewProj[16];    // Current frame inverse view-projection
    float prevViewProj[16];   // Previous frame view-projection
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float blurStrength;       // Motion blur intensity multiplier
    float maxBlurRadius;      // Maximum blur radius in pixels (default 32)
    float depthThreshold;     // Depth discontinuity threshold
    float padding;
};

// =============================================================================
// Bloom Constants - matches bloom_*_ps.hlsl cbuffers
// =============================================================================
struct BloomExtractConstants {
    float threshold;          // Brightness threshold (0.8 - 1.5)
    float softThreshold;      // Soft knee for smooth transition (0.0 - 0.5)
    float invWidth;           // 1/width
    float invHeight;          // 1/height
    float useKarisAverage;    // 1.0 to use Karis average for anti-firefly
    float padding1;
    float padding2;
    float padding3;
};

struct BloomDownsampleConstants {
    float invWidth;           // 1/width of INPUT texture
    float invHeight;          // 1/height of INPUT texture
    float mipLevel;           // Current mip level (0 = first downsample)
    float useKarisAverage;    // 1.0 to use Karis average for anti-firefly
    float padding[4];
};

struct BloomUpsampleConstants {
    float invWidth;           // 1/width of OUTPUT texture
    float invHeight;          // 1/height of OUTPUT texture
    float filterRadius;       // Filter radius in texels (0.5 - 1.0)
    float bloomIntensity;     // Bloom intensity for this pass
    float padding[4];
};

struct BloomCompositeConstants {
    float bloomIntensity;     // Overall bloom strength (0.0 - 2.0)
    float saturation;         // Bloom color saturation (0.0 - 2.0)
    float blendMode;          // 0 = additive, 1 = screen
    float padding;
};

// =============================================================================
// Sun Shafts Constants - matches sunshafts_*_ps.hlsl cbuffers
// =============================================================================
struct SunShaftsPrepassConstants {
    float sunPosX;            // Sun position in screen space UV X
    float sunPosY;            // Sun position in screen space UV Y
    float sunRadius;          // Sun disk radius in screen space
    float skyDepthThreshold;  // Depth values above this are sky (0.9999)
    float invWidth;           // 1/width
    float invHeight;          // 1/height
    float horizonFade;        // Horizon fade factor
    float padding;
};

struct SunShaftsRadialConstants {
    float sunPosX;            // Sun position in screen space UV X
    float sunPosY;            // Sun position in screen space UV Y
    float density;            // Ray density (0.5 - 1.0)
    float weight;             // Weight for each sample (0.5 - 1.0)
    float decay;              // How fast rays fade (0.9 - 0.99)
    float exposure;           // Exposure/brightness of rays (0.1 - 1.0)
    float numSamples;         // Number of ray march samples (16-64)
    float padding;
};

struct SunShaftsCompositeConstants {
    float resolutionX;        // 1/width
    float resolutionY;        // 1/height
    float width;
    float height;
    float sunColorR;          // Sun color tint
    float sunColorG;
    float sunColorB;
    float sunColorA;          // Intensity
    float blendStrength;      // Overall blend strength (0.0 - 1.0)
    float padding[3];
};

// =============================================================================
// Post-Process Renderer
// =============================================================================
class PostProcessRenderer {
public:
    PostProcessRenderer() = default;
    ~PostProcessRenderer();

    // Initialize renderer with device and pipeline layout
    bool Initialize(RenderDevice* device, RenderPipelineLayout* pipelineLayout,
                    RenderDescriptorSet* textureDescriptorSet,
                    uint32_t displayWidth, uint32_t displayHeight);
    
    // Shutdown and release resources
    void Shutdown();
    
    // Check if initialized
    bool IsInitialized() const { return m_initialized; }
    
    // Resize buffers when display size changes
    void Resize(uint32_t displayWidth, uint32_t displayHeight);
    
    // =======================================================================
    // TAA Implementation
    // =======================================================================
    
    // Apply TAA to color buffer
    // colorTexture: current frame color (input)
    // depthTexture: current frame depth
    // motionTexture: motion vectors (RG = velocity)
    // outputTexture: TAA result (output)
    // Returns true if TAA was applied
    bool ApplyTAA(RenderCommandList* commandList,
                  RenderTexture* colorTexture,
                  RenderTexture* depthTexture,
                  RenderTexture* motionTexture,
                  RenderTexture* outputTexture,
                  float jitterX, float jitterY,
                  float prevJitterX, float prevJitterY,
                  bool resetHistory);
    
    // =======================================================================
    // SMAA Implementation
    // =======================================================================
    
    // Apply SMAA to color buffer
    // colorTexture: input color
    // outputTexture: SMAA result
    // Returns true if SMAA was applied
    bool ApplySMAA(RenderCommandList* commandList,
                   RenderTexture* colorTexture,
                   RenderTexture* outputTexture);
    
    // =======================================================================
    // FSR 1.0 Implementation
    // =======================================================================
    
    // Apply FSR 1.0 upscaling
    // inputTexture: low-res input
    // outputTexture: high-res output
    // inputWidth/Height: input dimensions
    // outputWidth/Height: output dimensions
    // sharpness: 0.0 = soft, 1.0 = sharp
    bool ApplyFSR1(RenderCommandList* commandList,
                   RenderTexture* inputTexture,
                   RenderTexture* outputTexture,
                   uint32_t inputWidth, uint32_t inputHeight,
                   uint32_t outputWidth, uint32_t outputHeight,
                   float sharpness);
    
    // Get TAA history buffer for external use
    RenderTexture* GetTAAHistoryBuffer() const { return m_taaHistoryBuffer.get(); }
    
    // Swap TAA history buffers (call after TAA pass)
    void SwapTAAHistory();
    
    // =======================================================================
    // Vignette Implementation
    // =======================================================================
    
    // Apply vignette effect to color buffer
    // colorTexture: input color
    // outputTexture: vignette result
    // Uses config values for intensity, radius, softness, roundness
    // Returns true if vignette was applied
    bool ApplyVignette(RenderCommandList* commandList,
                       RenderTexture* colorTexture,
                       RenderTexture* outputTexture,
                       uint32_t textureDescriptorIndex);
    
    // =======================================================================
    // SSAO (Screen-Space Ambient Occlusion) Implementation
    // =======================================================================
    
    // Apply SSAO to the scene
    // depthTexture: depth buffer
    // colorTexture: scene color (for composite)
    // outputTexture: SSAO result
    // cameraNear/cameraFar: camera clipping planes
    // cameraFovY: vertical field of view in radians
    // Returns true if SSAO was applied
    bool ApplySSAO(RenderCommandList* commandList,
                   RenderTexture* depthTexture,
                   RenderTexture* colorTexture,
                   RenderTexture* outputTexture,
                   float cameraNear, float cameraFar, float cameraFovY);
    
    // =======================================================================
    // Depth of Field Implementation
    // =======================================================================
    
    // Apply Depth of Field effect
    // colorTexture: input scene color
    // depthTexture: depth buffer
    // outputTexture: DoF result
    // focusDistance: distance to focus plane
    // apertureSize: lens aperture (affects blur strength)
    // cameraNear/cameraFar: camera clipping planes
    // Returns true if DoF was applied
    bool ApplyDoF(RenderCommandList* commandList,
                  RenderTexture* colorTexture,
                  RenderTexture* depthTexture,
                  RenderTexture* outputTexture,
                  float focusDistance, float apertureSize,
                  float cameraNear, float cameraFar);
    
    // =======================================================================
    // SSR (Screen-Space Reflections) Implementation
    // =======================================================================
    
    // Apply Screen-Space Reflections
    // colorTexture: input scene color
    // depthTexture: depth buffer
    // outputTexture: SSR composite result
    // cameraNear/cameraFar/cameraFovY: camera parameters
    // viewMatrix/projMatrix: camera matrices (row-major, 16 floats each)
    // Returns true if SSR was applied
    bool ApplySSR(RenderCommandList* commandList,
                  RenderTexture* colorTexture,
                  RenderTexture* depthTexture,
                  RenderTexture* outputTexture,
                  float cameraNear, float cameraFar, float cameraFovY,
                  const float* viewMatrix, const float* projMatrix);
    
    // =======================================================================
    // Film Grain Implementation
    // =======================================================================
    
    // Apply film grain effect
    // colorTexture: input color
    // outputTexture: film grain result
    // Uses Config::FilmGrain and Config::FilmGrainIntensity
    // Returns true if film grain was applied
    bool ApplyFilmGrain(RenderCommandList* commandList,
                        RenderTexture* colorTexture,
                        RenderTexture* outputTexture,
                        uint32_t textureDescriptorIndex);
    
    // =======================================================================
    // Chromatic Aberration Implementation
    // =======================================================================
    
    // Apply chromatic aberration effect
    // colorTexture: input color
    // outputTexture: chromatic aberration result
    // Uses Config::ChromaticAberration and Config::ChromaticAberrationIntensity
    // Returns true if chromatic aberration was applied
    bool ApplyChromaticAberration(RenderCommandList* commandList,
                                   RenderTexture* colorTexture,
                                   RenderTexture* outputTexture,
                                   uint32_t textureDescriptorIndex);
    
    // =======================================================================
    // Motion Blur Implementation
    // =======================================================================
    
    // Apply camera motion blur effect
    // colorTexture: input color
    // depthTexture: depth buffer
    // outputTexture: motion blur result
    // invViewProj/prevViewProj: camera matrices for reprojection (row-major, 16 floats each)
    // Uses Config::MotionBlur and Config::MotionBlurStrength
    // Returns true if motion blur was applied
    bool ApplyMotionBlur(RenderCommandList* commandList,
                         RenderTexture* colorTexture,
                         RenderTexture* depthTexture,
                         RenderTexture* outputTexture,
                         const float* invViewProj, const float* prevViewProj);
    
    // =======================================================================
    // Bloom Implementation
    // =======================================================================
    
    // Apply bloom effect using mip chain pyramid
    // colorTexture: input HDR color
    // outputTexture: bloom composite result
    // Uses Config::BloomEnabled, Config::BloomThreshold, Config::BloomIntensity
    // Returns true if bloom was applied
    bool ApplyBloom(RenderCommandList* commandList,
                    RenderTexture* colorTexture,
                    RenderTexture* outputTexture,
                    float threshold = 0.0f,
                    float intensity = 0.0f);
    
    // =======================================================================
    // Sun Shafts (God Rays) Implementation
    // =======================================================================
    
    // Apply volumetric light scattering (god rays)
    // colorTexture: input HDR color
    // depthTexture: depth buffer
    // outputTexture: sun shafts composite result
    // sunScreenPos: sun position in screen UV space (z < 0 if behind camera)
    // sunDirection: sun direction in world space
    // sunColor: sun color tint (RGB, A = intensity)
    // Uses Config::SunShaftsEnabled, Config::SunShaftsDensity, etc.
    // Returns true if sun shafts were applied
    bool ApplySunShafts(RenderCommandList* commandList,
                        RenderTexture* colorTexture,
                        RenderTexture* depthTexture,
                        RenderTexture* outputTexture,
                        float sunScreenX, float sunScreenY,
                        float density, float weight, float decay, float exposure);

private:
    bool CreateShaders();
    bool CreatePipelines();
    bool CreateRenderTargets(uint32_t width, uint32_t height);
    bool CreateSMAATables();
    
    void DrawFullscreenTriangle(RenderCommandList* commandList);
    
    bool m_initialized = false;
    RenderDevice* m_device = nullptr;
    RenderPipelineLayout* m_pipelineLayout = nullptr;
    RenderDescriptorSet* m_textureDescriptorSet = nullptr;
    
    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;
    
    // Shaders
    std::unique_ptr<RenderShader> m_fullscreenVS;
    std::unique_ptr<RenderShader> m_taaPS;
    std::unique_ptr<RenderShader> m_smaaEdgePS;
    std::unique_ptr<RenderShader> m_smaaBlendPS;
    std::unique_ptr<RenderShader> m_smaaNeighborhoodBlendPS;
    std::unique_ptr<RenderShader> m_fsr1EasuPS;
    std::unique_ptr<RenderShader> m_fsr1RcasPS;
    std::unique_ptr<RenderShader> m_vignettePS;
    std::unique_ptr<RenderShader> m_ssaoPS;
    std::unique_ptr<RenderShader> m_ssaoBlurPS;
    std::unique_ptr<RenderShader> m_ssaoCompositePS;
    std::unique_ptr<RenderShader> m_dofPrefilterPS;     // DoF prefilter (downsample)
    std::unique_ptr<RenderShader> m_dofBokehPS;         // DoF bokeh (blur)
    std::unique_ptr<RenderShader> m_dofPostfilterPS;    // DoF postfilter (tent filter)
    std::unique_ptr<RenderShader> m_dofCombinePS;       // DoF combine (final composite)
    std::unique_ptr<RenderShader> m_ssrRaytracePS;
    std::unique_ptr<RenderShader> m_ssrCompositePS;
    std::unique_ptr<RenderShader> m_filmGrainPS;
    std::unique_ptr<RenderShader> m_chromaticAberrationPS;
    std::unique_ptr<RenderShader> m_motionBlurCameraPS;
    // Bloom shaders
    std::unique_ptr<RenderShader> m_bloomExtractPS;
    std::unique_ptr<RenderShader> m_bloomDownsamplePS;
    std::unique_ptr<RenderShader> m_bloomUpsamplePS;
    std::unique_ptr<RenderShader> m_bloomCompositePS;
    // Sun shafts shaders
    std::unique_ptr<RenderShader> m_sunShaftsPrepassPS;
    std::unique_ptr<RenderShader> m_sunShaftsRadialPS;
    std::unique_ptr<RenderShader> m_sunShaftsCompositePS;
    
    // Pipelines
    std::unique_ptr<RenderPipeline> m_taaPipeline;
    std::unique_ptr<RenderPipeline> m_smaaEdgePipeline;
    std::unique_ptr<RenderPipeline> m_smaaBlendPipeline;
    std::unique_ptr<RenderPipeline> m_smaaNeighborhoodBlendPipeline;
    std::unique_ptr<RenderPipeline> m_fsr1EasuPipeline;
    std::unique_ptr<RenderPipeline> m_fsr1RcasPipeline;
    std::unique_ptr<RenderPipeline> m_vignettePipeline;
    std::unique_ptr<RenderPipeline> m_ssaoPipeline;
    std::unique_ptr<RenderPipeline> m_ssaoBlurPipeline;
    std::unique_ptr<RenderPipeline> m_ssaoCompositePipeline;
    std::unique_ptr<RenderPipeline> m_dofPrefilterPipeline;    // DoF pass 1
    std::unique_ptr<RenderPipeline> m_dofBokehPipeline;        // DoF pass 2
    std::unique_ptr<RenderPipeline> m_dofPostfilterPipeline;   // DoF pass 3
    std::unique_ptr<RenderPipeline> m_dofCombinePipeline;      // DoF pass 4
    std::unique_ptr<RenderPipeline> m_ssrRaytracePipeline;
    std::unique_ptr<RenderPipeline> m_ssrCompositePipeline;
    std::unique_ptr<RenderPipeline> m_filmGrainPipeline;
    std::unique_ptr<RenderPipeline> m_chromaticAberrationPipeline;
    std::unique_ptr<RenderPipeline> m_motionBlurCameraPipeline;
    // Bloom pipelines
    std::unique_ptr<RenderPipeline> m_bloomExtractPipeline;
    std::unique_ptr<RenderPipeline> m_bloomDownsamplePipeline;
    std::unique_ptr<RenderPipeline> m_bloomUpsamplePipeline;
    std::unique_ptr<RenderPipeline> m_bloomCompositePipeline;
    // Sun shafts pipelines
    std::unique_ptr<RenderPipeline> m_sunShaftsPrepassPipeline;
    std::unique_ptr<RenderPipeline> m_sunShaftsRadialPipeline;
    std::unique_ptr<RenderPipeline> m_sunShaftsCompositePipeline;
    
    // TAA history buffers (double-buffered)
    std::unique_ptr<RenderTexture> m_taaHistoryBuffer;
    std::unique_ptr<RenderTexture> m_taaHistoryBufferPrev;
    std::unique_ptr<RenderFramebuffer> m_taaFramebuffer;
    uint32_t m_taaHistoryDescriptorIndex = 0;
    uint32_t m_taaHistoryPrevDescriptorIndex = 0;
    
    // SMAA intermediate buffers
    std::unique_ptr<RenderTexture> m_smaaEdgeBuffer;
    std::unique_ptr<RenderTexture> m_smaaBlendBuffer;
    std::unique_ptr<RenderFramebuffer> m_smaaEdgeFramebuffer;
    std::unique_ptr<RenderFramebuffer> m_smaaBlendFramebuffer;
    uint32_t m_smaaEdgeDescriptorIndex = 0;
    uint32_t m_smaaBlendDescriptorIndex = 0;
    
    // SMAA lookup tables (area and search textures)
    std::unique_ptr<RenderTexture> m_smaaAreaTex;
    std::unique_ptr<RenderTexture> m_smaaSearchTex;
    uint32_t m_smaaAreaDescriptorIndex = 0;
    uint32_t m_smaaSearchDescriptorIndex = 0;
    
    // FSR 1.0 intermediate buffer (for EASU output before RCAS)
    std::unique_ptr<RenderTexture> m_fsr1IntermediateBuffer;
    std::unique_ptr<RenderFramebuffer> m_fsr1IntermediateFramebuffer;
    uint32_t m_fsr1IntermediateDescriptorIndex = 0;
    
    // Samplers
    std::unique_ptr<RenderSampler> m_linearSampler;
    std::unique_ptr<RenderSampler> m_pointSampler;
    
    // Constant buffers for shader parameters
    std::unique_ptr<RenderBuffer> m_taaConstantBuffer;
    std::unique_ptr<RenderBuffer> m_smaaConstantBuffer;
    std::unique_ptr<RenderBuffer> m_fsr1ConstantBuffer;
    std::unique_ptr<RenderBuffer> m_ssaoConstantBuffer;
    std::unique_ptr<RenderBuffer> m_ssaoBlurConstantBuffer;
    std::unique_ptr<RenderBuffer> m_dofConstantBuffer;
    
    // SSAO intermediate buffers
    std::unique_ptr<RenderTexture> m_ssaoBuffer;           // Raw AO output
    std::unique_ptr<RenderTexture> m_ssaoBlurBuffer;       // Horizontally blurred AO
    std::unique_ptr<RenderFramebuffer> m_ssaoFramebuffer;
    std::unique_ptr<RenderFramebuffer> m_ssaoBlurFramebuffer;
    
    // DoF intermediate buffers (half-resolution)
    std::unique_ptr<RenderTexture> m_dofHalfBuffer0;       // Prefilter output / Bokeh input
    std::unique_ptr<RenderTexture> m_dofHalfBuffer1;       // Bokeh output / Postfilter input
    std::unique_ptr<RenderFramebuffer> m_dofHalfFramebuffer0;
    std::unique_ptr<RenderFramebuffer> m_dofHalfFramebuffer1;
    
    // SSR intermediate buffers
    std::unique_ptr<RenderTexture> m_ssrBuffer;           // Reflection result
    std::unique_ptr<RenderFramebuffer> m_ssrFramebuffer;
    std::unique_ptr<RenderBuffer> m_ssrConstantBuffer;
    std::unique_ptr<RenderBuffer> m_ssrCompositeConstantBuffer;
    
    // Film Grain, Chromatic Aberration, Motion Blur constant buffers
    std::unique_ptr<RenderBuffer> m_filmGrainConstantBuffer;
    std::unique_ptr<RenderBuffer> m_chromaticAberrationConstantBuffer;
    std::unique_ptr<RenderBuffer> m_motionBlurConstantBuffer;
    
    // Bloom mip chain buffers (6 levels: 1/2, 1/4, 1/8, 1/16, 1/32, 1/64)
    static constexpr int BLOOM_MIP_COUNT = 6;
    std::unique_ptr<RenderTexture> m_bloomMipChain[BLOOM_MIP_COUNT];
    std::unique_ptr<RenderFramebuffer> m_bloomMipFramebuffers[BLOOM_MIP_COUNT];
    std::unique_ptr<RenderTexture> m_bloomResult;  // Final combined bloom
    std::unique_ptr<RenderFramebuffer> m_bloomResultFramebuffer;
    std::unique_ptr<RenderBuffer> m_bloomExtractConstantBuffer;
    std::unique_ptr<RenderBuffer> m_bloomDownsampleConstantBuffer;
    std::unique_ptr<RenderBuffer> m_bloomUpsampleConstantBuffer;
    std::unique_ptr<RenderBuffer> m_bloomCompositeConstantBuffer;
    
    // Sun shafts intermediate buffers (half resolution)
    std::unique_ptr<RenderTexture> m_sunShaftsPrepassBuffer;  // Sun/sky extraction
    std::unique_ptr<RenderTexture> m_sunShaftsRadialBuffer;   // Radial blur result
    std::unique_ptr<RenderFramebuffer> m_sunShaftsPrepassFramebuffer;
    std::unique_ptr<RenderFramebuffer> m_sunShaftsRadialFramebuffer;
    std::unique_ptr<RenderBuffer> m_sunShaftsPrepassConstantBuffer;
    std::unique_ptr<RenderBuffer> m_sunShaftsRadialConstantBuffer;
    std::unique_ptr<RenderBuffer> m_sunShaftsCompositeConstantBuffer;
    
    // Store previous view-projection matrix for motion blur
    float m_prevViewProj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    
    // Blue noise texture for SSAO temporal jitter
    std::unique_ptr<RenderTexture> m_blueNoiseTex;
    
    // Frame counter for history management
    uint32_t m_frameIndex = 0;
};

// Global post-process renderer instance
extern PostProcessRenderer g_postProcessRenderer;

// Initialize the global renderer (called from Video::CreateHostDevice)
bool InitializePostProcessRenderer(RenderDevice* device, RenderPipelineLayout* pipelineLayout,
                                    RenderDescriptorSet* textureDescriptorSet,
                                    uint32_t displayWidth, uint32_t displayHeight);

// Shutdown the global renderer
void ShutdownPostProcessRenderer();

} // namespace PostProcess
