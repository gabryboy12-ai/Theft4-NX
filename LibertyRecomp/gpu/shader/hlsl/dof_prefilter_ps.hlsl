// =============================================================================
// Depth of Field - Prefilter Pass
// FusionFix-style multi-pass DoF implementation
// 
// This pass:
// 1. Downsamples color to half resolution with brightness-weighted averaging
// 2. Downsamples CoC with extreme value selection (preserves FG/BG separation)
// 3. Outputs: RGB = downsampled color, A = CoC value
// =============================================================================

Texture2D g_colorTex : register(t0);
Texture2D g_depthTex : register(t1);
SamplerState g_linearSampler : register(s0);
SamplerState g_pointSampler : register(s1);

cbuffer DOFConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height) - full res
    float4 g_halfResolution;  // (1/halfWidth, 1/halfHeight, halfWidth, halfHeight)
    float g_focusDistance;    // Distance to focus plane
    float g_focusRange;       // Range where things are in focus
    float g_nearPlane;        // Camera near plane
    float g_farPlane;         // Camera far plane
    float g_bokehRadius;      // Maximum bokeh radius in half-res texels
    float g_maxBlur;          // Maximum blur radius in pixels
    float g_cocScale;         // CoC scale factor for visibility
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Convert depth buffer value to linear depth
float LinearizeDepth(float depth)
{
    float z = depth;
    float denominator = g_farPlane + g_nearPlane - z * (g_farPlane - g_nearPlane);
    denominator = max(abs(denominator), 0.0001) * sign(denominator + 0.0001);
    return (2.0 * g_nearPlane * g_farPlane) / denominator;
}

// Calculate signed CoC: positive = background (far), negative = foreground (near)
// CoC is scaled by bokeh radius for direct use in bokeh pass
float CalculateCoC(float depth)
{
    float linearDepth = LinearizeDepth(depth);
    linearDepth = max(linearDepth, 0.001);
    
    // Signed CoC: negative for foreground, positive for background
    float coc = (linearDepth - g_focusDistance) / g_focusRange;
    
    // Clamp to [-1, 1] range and scale by bokeh radius
    coc = clamp(coc, -1.0, 1.0) * g_bokehRadius;
    
    return coc;
}

// Weight for brightness preservation during downsample
// Prevents bright bokeh from "exploding" in size
float WeighByBrightness(float3 color)
{
    float maxChannel = max(max(color.r, color.g), color.b);
    return 1.0 / (1.0 + maxChannel);
}

float4 shaderMain(PSInput input) : SV_Target
{
    // We're rendering at half resolution, so texCoord is in half-res space
    // But we need to sample from full-res textures
    float2 uv = input.texCoord;
    
    // Calculate offsets for 2x2 tap pattern (half texel offset in full-res space)
    // This samples the 4 full-res texels that correspond to this half-res texel
    float4 offsets = g_resolution.xyxy * float2(-0.5, 0.5).xxyy;
    
    // Sample 4 colors from full-res texture with brightness weighting
    float3 s0 = g_colorTex.SampleLevel(g_linearSampler, uv + offsets.xy, 0).rgb;
    float3 s1 = g_colorTex.SampleLevel(g_linearSampler, uv + offsets.zy, 0).rgb;
    float3 s2 = g_colorTex.SampleLevel(g_linearSampler, uv + offsets.xw, 0).rgb;
    float3 s3 = g_colorTex.SampleLevel(g_linearSampler, uv + offsets.zw, 0).rgb;
    
    // Weight by inverse brightness to prevent bright areas from dominating
    float w0 = WeighByBrightness(s0);
    float w1 = WeighByBrightness(s1);
    float w2 = WeighByBrightness(s2);
    float w3 = WeighByBrightness(s3);
    
    // Weighted average for color
    float3 color = s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3;
    color /= max(w0 + w1 + w2 + w3, 0.00001);
    
    // Sample 4 depths and calculate CoC values
    float coc0 = CalculateCoC(g_depthTex.SampleLevel(g_pointSampler, uv + offsets.xy, 0).r);
    float coc1 = CalculateCoC(g_depthTex.SampleLevel(g_pointSampler, uv + offsets.zy, 0).r);
    float coc2 = CalculateCoC(g_depthTex.SampleLevel(g_pointSampler, uv + offsets.xw, 0).r);
    float coc3 = CalculateCoC(g_depthTex.SampleLevel(g_pointSampler, uv + offsets.zw, 0).r);
    
    // Select most extreme CoC value (not average!)
    // This preserves sharp foreground edges and prevents bleeding
    float cocMin = min(min(min(coc0, coc1), coc2), coc3);
    float cocMax = max(max(max(coc0, coc1), coc2), coc3);
    
    // Use the most extreme value: if max is larger than |min|, use max, else use min
    float coc = (cocMax >= -cocMin) ? cocMax : cocMin;
    
    return float4(color, coc);
}
