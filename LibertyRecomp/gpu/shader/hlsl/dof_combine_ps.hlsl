// =============================================================================
// Depth of Field - Combine Pass
// FusionFix-style multi-pass DoF implementation
// 
// This pass combines the bokeh result with the sharp source image.
// In-focus pixels use the sharp source, out-of-focus pixels use bokeh.
// The foreground alpha from bokeh pass is used to ensure foreground
// properly occludes background even when background is in focus.
// =============================================================================

Texture2D g_colorTex : register(t0);     // Full-res sharp source
Texture2D g_depthTex : register(t1);     // Full-res depth buffer
Texture2D g_dofTex : register(t2);       // Half-res bokeh result (RGB + FG alpha)
SamplerState g_linearSampler : register(s0);
SamplerState g_pointSampler : register(s1);

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

// Convert depth buffer value to linear depth
float LinearizeDepth(float depth)
{
    float z = depth;
    float denominator = g_farPlane + g_nearPlane - z * (g_farPlane - g_nearPlane);
    denominator = max(abs(denominator), 0.0001) * sign(denominator + 0.0001);
    return (2.0 * g_nearPlane * g_farPlane) / denominator;
}

// Calculate signed CoC for this pixel
float CalculateCoC(float depth)
{
    float linearDepth = LinearizeDepth(depth);
    linearDepth = max(linearDepth, 0.001);
    
    // Signed CoC in [-1, 1] range
    float coc = (linearDepth - g_focusDistance) / g_focusRange;
    return clamp(coc, -1.0, 1.0);
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 uv = input.texCoord;
    
    // Sample full-res sharp source
    float4 source = g_colorTex.SampleLevel(g_linearSampler, uv, 0);
    
    // Sample full-res depth and calculate CoC
    float depth = g_depthTex.SampleLevel(g_pointSampler, uv, 0).r;
    float coc = CalculateCoC(depth);
    
    // Sample half-res DoF result (upsampled via bilinear)
    float4 dof = g_dofTex.SampleLevel(g_linearSampler, uv, 0);
    float fgAlpha = dof.a;  // Foreground blend factor from bokeh pass
    
    // Calculate DoF strength based on absolute CoC
    // - abs(coc) < 0.1: fully in focus -> use source
    // - abs(coc) > 1.0: fully out of focus -> use dof
    // - between: blend smoothly
    float dofStrength = smoothstep(0.1, 1.0, abs(coc));
    
    // Combine DoF strength with foreground alpha
    // This ensures foreground properly occludes background even when
    // the background pixel itself is in focus
    // Formula: dofStrength + fgAlpha - dofStrength * fgAlpha
    // This is a nonlinear blend that makes DoF slightly stronger
    float finalBlend = dofStrength + fgAlpha - dofStrength * fgAlpha;
    
    // Final composite: lerp between sharp source and bokeh result
    float3 color = lerp(source.rgb, dof.rgb, finalBlend);
    
    return float4(color, source.a);
}
