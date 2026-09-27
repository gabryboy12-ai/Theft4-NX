// =============================================================================
// Depth of Field - Bokeh Pass
// FusionFix-style multi-pass DoF implementation
// 
// This pass creates the bokeh effect using disk kernel sampling
// with proper foreground/background separation to prevent bleeding.
// Input: Half-res prefiltered texture (RGB = color, A = signed CoC)
// Output: Half-res bokeh (RGB = blurred color, A = foreground weight)
// =============================================================================

Texture2D g_prefilterTex : register(t0);  // Prefiltered half-res (RGB = color, A = CoC)
SamplerState g_linearSampler : register(s0);

cbuffer DOFConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height) - full res
    float4 g_halfResolution;  // (1/halfWidth, 1/halfHeight, halfWidth, halfHeight)
    float g_focusDistance;
    float g_focusRange;
    float g_nearPlane;
    float g_farPlane;
    float g_bokehRadius;      // Maximum bokeh radius in half-res texels
    float g_maxBlur;
    float g_cocScale;
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Medium quality disk kernel (22 samples)
// From Unity Post Processing Stack v2
// Samples arranged in 2 rings + center for good coverage
static const int KERNEL_SAMPLE_COUNT = 22;
static const float2 KERNEL[22] = {
    float2(0, 0),
    float2(0.53333336, 0),
    float2(0.3325279, 0.4169768),
    float2(-0.11867785, 0.5199616),
    float2(-0.48051673, 0.2314047),
    float2(-0.48051673, -0.23140468),
    float2(-0.11867763, -0.51996166),
    float2(0.33252785, -0.4169769),
    float2(1, 0),
    float2(0.90096885, 0.43388376),
    float2(0.6234898, 0.7818315),
    float2(0.22252098, 0.9749279),
    float2(-0.22252095, 0.9749279),
    float2(-0.62349, 0.7818314),
    float2(-0.90096885, 0.43388382),
    float2(-1, 0),
    float2(-0.90096885, -0.43388376),
    float2(-0.6234896, -0.7818316),
    float2(-0.22252055, -0.974928),
    float2(0.2225215, -0.9749278),
    float2(0.6234897, -0.7818316),
    float2(0.90096885, -0.43388376)
};

// Smooth weighting function for sample inclusion
// Uses soft ramp instead of hard threshold for smoother transitions
float WeighSample(float coc, float radius)
{
    // Soft ramp: samples are fully included when CoC >= radius,
    // partially included when CoC is slightly smaller
    return saturate((coc - radius + 2.0) / 2.0);
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 uv = input.texCoord;
    float2 texelSize = g_halfResolution.xy;
    
    // Sample center pixel
    float4 centerSample = g_prefilterTex.SampleLevel(g_linearSampler, uv, 0);
    float centerCoC = centerSample.a;
    
    // Separate accumulation for foreground and background
    float3 bgColor = float3(0, 0, 0);
    float bgWeight = 0.0;
    float3 fgColor = float3(0, 0, 0);
    float fgWeight = 0.0;
    
    // Sample all kernel positions
    [unroll]
    for (int k = 0; k < KERNEL_SAMPLE_COUNT; k++)
    {
        // Scale kernel offset by bokeh radius
        float2 offset = KERNEL[k] * g_bokehRadius;
        float radius = length(offset);
        offset *= texelSize;
        
        // Sample prefiltered texture
        float4 s = g_prefilterTex.SampleLevel(g_linearSampler, uv + offset, 0);
        float sampleCoC = s.a;
        
        // ===================================================================
        // Background accumulation (positive CoC = far from camera)
        // ===================================================================
        // Use max of 0 and sample CoC, clamped by center CoC to prevent
        // background bleeding onto sharp foreground
        float bgCoc = max(0.0, min(sampleCoC, centerCoC));
        float bgw = WeighSample(bgCoc, radius);
        bgColor += s.rgb * bgw;
        bgWeight += bgw;
        
        // ===================================================================
        // Foreground accumulation (negative CoC = near to camera)
        // ===================================================================
        // Use -CoC (making it positive) for foreground weighting
        float fgCoc = -sampleCoC;  // Convert negative CoC to positive
        float fgw = WeighSample(fgCoc, radius);
        fgColor += s.rgb * fgw;
        fgWeight += fgw;
    }
    
    // Normalize colors (avoid division by zero)
    bgColor = bgColor / max(bgWeight, 0.001);
    fgColor = fgColor / max(fgWeight, 0.001);
    
    // Blend foreground and background based on foreground weight
    // Foreground dominates when we have significant FG samples
    // Use PI to boost foreground contribution (matches Unity's implementation)
    float bgfg = saturate(fgWeight * 3.14159265 / KERNEL_SAMPLE_COUNT);
    
    float3 color = lerp(bgColor, fgColor, bgfg);
    
    // Output: RGB = blended bokeh color, A = foreground/background blend factor
    return float4(color, bgfg);
}
