// Sun Shafts (God Rays) Prepass Pixel Shader
// Extracts sun/sky pixels and masks out occluders for volumetric light scattering
//
// Reference: GPU Gems 3 - Chapter 13 "Volumetric Light Scattering as a Post-Process"
// Reference: FusionFix SunShafts_PS.hlsl

Texture2D g_colorTex : register(t0);    // HDR scene color
Texture2D g_depthTex : register(t1);    // Scene depth buffer
SamplerState g_linearSampler : register(s0);
SamplerState g_pointSampler : register(s1);

cbuffer SunShaftsPrepassConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float4 g_sunScreenPos;    // Sun position in screen space (xy = UV, zw = valid flag)
    float4 g_sunDirection;    // Sun direction in world space (xyz, w = intensity)
    float g_threshold;        // Brightness threshold for sun extraction
    float g_skyDepth;         // Depth value considered as sky (0.9999)
    float g_sunMaskRadius;    // Radius for sun disk mask
    float g_intensity;        // Overall intensity multiplier
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Check if this pixel is sky (far plane)
float SkyMask(float depth, float skyDepth)
{
    return depth >= skyDepth ? 1.0 : 0.0;
}

// Sun disk mask - creates soft falloff around sun position
float SunDiskMask(float2 uv, float2 sunPos, float radius)
{
    float dist = length(uv - sunPos);
    return smoothstep(radius, radius * 0.5, dist);
}

// Edge fade to prevent artifacts at screen borders
float EdgeFade(float2 uv, float aspectRatio)
{
    float2 edge = min(uv, 1.0 - uv) * float2(aspectRatio, 1.0);
    return saturate(32.0 * min(edge.x, edge.y));
}

// Horizon fade - reduce intensity near horizon (sunrise/sunset)
float HorizonFade(float3 sunDir)
{
    // Fade based on sun height above horizon
    float azimuth = smoothstep(0.0, 0.21, sunDir.z);
    return azimuth;
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    
    // Skip if sun is behind camera
    if (g_sunScreenPos.z < 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    
    // Sample scene color and depth
    float3 color = g_colorTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    float depth = g_depthTex.SampleLevel(g_pointSampler, texCoord, 0).r;
    
    // Only process sky pixels
    float skyMask = SkyMask(depth, g_skyDepth);
    
    // Extract bright pixels that exceed threshold
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float brightMask = saturate((luminance - g_threshold) / (1.0 - g_threshold));
    
    // Apply sun disk enhancement near sun position
    float sunDisk = SunDiskMask(texCoord, g_sunScreenPos.xy, g_sunMaskRadius);
    
    // Combine masks
    float mask = skyMask * (brightMask + sunDisk * 0.5);
    
    // Edge fade
    float aspectRatio = g_resolution.z / g_resolution.w;
    mask *= EdgeFade(texCoord, aspectRatio);
    
    // Horizon fade (optional - helps with sunrise/sunset)
    mask *= HorizonFade(g_sunDirection.xyz);
    
    // Apply intensity
    float3 result = color * mask * g_intensity;
    
    return float4(result, 1.0);
}
