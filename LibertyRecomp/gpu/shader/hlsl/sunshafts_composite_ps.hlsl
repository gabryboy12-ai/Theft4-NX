// Sun Shafts Composite Pixel Shader
// Blends sun shafts with the original scene
// Uses additive blending with optional color tint

Texture2D g_sceneTex : register(t0);      // Original scene color
Texture2D g_sunShaftsTex : register(t1);  // Radial blur result
SamplerState g_linearSampler : register(s0);

cbuffer SunShaftsCompositeConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float4 g_sunColor;        // Sun color tint (RGB, A = intensity)
    float g_blendStrength;    // Overall blend strength (0.0 - 1.0)
    float3 g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    
    // Sample scene and sun shafts
    float3 scene = g_sceneTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    float3 shafts = g_sunShaftsTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    
    // Apply sun color tint
    shafts *= g_sunColor.rgb * g_sunColor.a;
    
    // Apply blend strength
    shafts *= g_blendStrength;
    
    // Additive blend
    float3 result = scene + shafts;
    
    return float4(result, 1.0);
}
