// =============================================================================
// Depth of Field - Post Filter Pass (Tent Filter)
// FusionFix-style multi-pass DoF implementation
// 
// This pass applies a 3x3 tent filter to smooth the bokeh samples.
// The tent filter uses 4 samples with half-texel offset, creating
// overlapping coverage that smooths the discrete kernel samples.
// Input/Output: Half-res bokeh texture
// =============================================================================

Texture2D g_bokehTex : register(t0);  // Bokeh output from previous pass
SamplerState g_linearSampler : register(s0);

cbuffer DOFConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height) - full res
    float4 g_halfResolution;  // (1/halfWidth, 1/halfHeight, halfWidth, halfHeight)
    float g_focusDistance;
    float g_focusRange;
    float g_nearPlane;
    float g_farPlane;
    float g_bokehRadius;
    float g_maxBlur;
    float g_cocScale;
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 shaderMain(PSInput input) : SV_Target
{
    float2 uv = input.texCoord;
    
    // Tent filter: 4 samples at half-texel offsets
    // This creates a 3x3 weighted kernel through bilinear interpolation:
    //
    //   [1 2 1]
    //   [2 4 2] / 16
    //   [1 2 1]
    //
    // But we achieve this with just 4 texture samples due to bilinear filtering
    
    float4 offsets = g_halfResolution.xyxy * float2(-0.5, 0.5).xxyy;
    
    float4 s0 = g_bokehTex.SampleLevel(g_linearSampler, uv + offsets.xy, 0);  // top-left
    float4 s1 = g_bokehTex.SampleLevel(g_linearSampler, uv + offsets.zy, 0);  // top-right
    float4 s2 = g_bokehTex.SampleLevel(g_linearSampler, uv + offsets.xw, 0);  // bottom-left
    float4 s3 = g_bokehTex.SampleLevel(g_linearSampler, uv + offsets.zw, 0);  // bottom-right
    
    // Simple average of the 4 samples
    // Bilinear filtering already does the weighted combination
    return (s0 + s1 + s2 + s3) * 0.25;
}
