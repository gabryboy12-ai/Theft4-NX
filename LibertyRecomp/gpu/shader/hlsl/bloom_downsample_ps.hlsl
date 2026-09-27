// Bloom Downsample Pixel Shader
// Progressive downsample for bloom mip chain
// Uses 13-tap filter for high quality downsampling that preserves energy
//
// Reference: "Next Generation Post Processing" by Jorge Jimenez (Call of Duty: Advanced Warfare)
// This filter samples a 4x4 region with optimal bilinear tap placement

Texture2D g_colorTex : register(t0);
SamplerState g_linearSampler : register(s0);

cbuffer BloomDownsampleConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height) of INPUT texture
    float g_mipLevel;         // Current mip level (0 = first downsample)
    float3 g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Karis average for first mip (anti-firefly)
float3 KarisAverage(float3 c1, float3 c2, float3 c3, float3 c4)
{
    float luma1 = dot(c1, float3(0.2126, 0.7152, 0.0722));
    float luma2 = dot(c2, float3(0.2126, 0.7152, 0.0722));
    float luma3 = dot(c3, float3(0.2126, 0.7152, 0.0722));
    float luma4 = dot(c4, float3(0.2126, 0.7152, 0.0722));
    
    float w1 = 1.0 / (1.0 + luma1);
    float w2 = 1.0 / (1.0 + luma2);
    float w3 = 1.0 / (1.0 + luma3);
    float w4 = 1.0 / (1.0 + luma4);
    
    return (c1 * w1 + c2 * w2 + c3 * w3 + c4 * w4) / (w1 + w2 + w3 + w4);
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    float2 texelSize = g_resolution.xy;
    
    // 13-tap filter: samples a 4x4 region optimally
    // Layout (x = sample position):
    //
    //   x . x . x
    //   . x . x .
    //   x . x . x
    //   . x . x .
    //   x . x . x
    //
    // This captures more area than a simple 2x2 box filter while
    // still being cache-efficient due to bilinear filtering
    
    // Center
    float3 center = g_colorTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    
    // 4 corners (2 texels out)
    float3 tl = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-2.0, -2.0) * texelSize, 0).rgb;
    float3 tr = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 2.0, -2.0) * texelSize, 0).rgb;
    float3 bl = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-2.0,  2.0) * texelSize, 0).rgb;
    float3 br = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 2.0,  2.0) * texelSize, 0).rgb;
    
    // 4 sides (2 texels out, centered)
    float3 top = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 0.0, -2.0) * texelSize, 0).rgb;
    float3 bot = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 0.0,  2.0) * texelSize, 0).rgb;
    float3 lft = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-2.0,  0.0) * texelSize, 0).rgb;
    float3 rgt = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 2.0,  0.0) * texelSize, 0).rgb;
    
    // 4 inner samples (1 texel out, diagonal)
    float3 itl = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-1.0, -1.0) * texelSize, 0).rgb;
    float3 itr = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 1.0, -1.0) * texelSize, 0).rgb;
    float3 ibl = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-1.0,  1.0) * texelSize, 0).rgb;
    float3 ibr = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( 1.0,  1.0) * texelSize, 0).rgb;
    
    float3 result;
    
    if (g_mipLevel == 0.0)
    {
        // First downsample uses Karis average to prevent fireflies
        float3 group1 = KarisAverage(itl, itr, ibl, ibr);
        float3 group2 = KarisAverage(tl, top, lft, center);
        float3 group3 = KarisAverage(top, tr, center, rgt);
        float3 group4 = KarisAverage(lft, center, bl, bot);
        float3 group5 = KarisAverage(center, rgt, bot, br);
        
        result = group1 * 0.5 + (group2 + group3 + group4 + group5) * 0.125;
    }
    else
    {
        // Subsequent downsamples use weighted average
        // Weights: center = 4, inner = 2, outer = 1
        result = center * 4.0;
        result += (itl + itr + ibl + ibr) * 2.0;
        result += tl + tr + bl + br + top + bot + lft + rgt;
        result /= 20.0; // Total weight: 4 + 4*2 + 8*1 = 20
    }
    
    return float4(result, 1.0);
}
