// Bloom Composite Pixel Shader
// Final pass that blends bloom result with original scene
// Supports additive and screen blend modes
//
// Reference: Standard HDR bloom pipeline

Texture2D g_sceneTex : register(t0);    // Original scene color
Texture2D g_bloomTex : register(t1);    // Final bloom result (upsampled to full res)
SamplerState g_linearSampler : register(s0);

cbuffer BloomCompositeConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float g_bloomIntensity;   // Overall bloom strength (0.0 - 2.0)
    float g_bloomSaturation;  // Bloom color saturation (0.0 - 2.0)
    float g_blendMode;        // 0 = additive, 1 = screen
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Adjust bloom saturation
float3 AdjustSaturation(float3 color, float saturation)
{
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(float3(luma, luma, luma), color, saturation);
}

// Screen blend mode: 1 - (1-a)(1-b)
// Lighter than additive, prevents over-brightening
float3 ScreenBlend(float3 base, float3 blend)
{
    return 1.0 - (1.0 - base) * (1.0 - blend);
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    
    // Sample scene and bloom
    float3 scene = g_sceneTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    float3 bloom = g_bloomTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    
    // Apply bloom saturation adjustment
    bloom = AdjustSaturation(bloom, g_bloomSaturation);
    
    // Apply intensity
    bloom *= g_bloomIntensity;
    
    // Blend modes
    float3 result;
    if (g_blendMode < 0.5)
    {
        // Additive blend (HDR-friendly)
        result = scene + bloom;
    }
    else
    {
        // Screen blend (softer, prevents harsh over-brightening)
        result = ScreenBlend(scene, bloom);
    }
    
    return float4(result, 1.0);
}
