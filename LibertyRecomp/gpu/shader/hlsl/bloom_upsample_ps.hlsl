// Bloom Upsample Pixel Shader
// Progressive upsample with 9-tap tent filter for smooth bloom
// Combines with higher resolution mip for additive accumulation
//
// Reference: "Next Generation Post Processing" by Jorge Jimenez
// Uses tent filter (bilinear-like weights) for smooth upsampling

Texture2D g_lowerMipTex : register(t0);   // Lower resolution bloom mip (being upsampled)
Texture2D g_higherMipTex : register(t1);  // Higher resolution bloom mip (to accumulate with)
SamplerState g_linearSampler : register(s0);

cbuffer BloomUpsampleConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height) of OUTPUT texture
    float g_filterRadius;     // Filter radius in texels (0.5 - 1.0 typical)
    float g_blendFactor;      // Blend between lower and higher mip (0.5 - 0.7)
    float2 g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// 9-tap tent filter for smooth upsampling
// Samples in a 3x3 pattern with bilinear weights
float3 TentFilter9Tap(Texture2D tex, SamplerState samp, float2 uv, float2 texelSize, float radius)
{
    float2 offset = texelSize * radius;
    
    // Sample 3x3 grid
    float3 tl = tex.SampleLevel(samp, uv + float2(-offset.x, -offset.y), 0).rgb;
    float3 tc = tex.SampleLevel(samp, uv + float2(      0.0, -offset.y), 0).rgb;
    float3 tr = tex.SampleLevel(samp, uv + float2( offset.x, -offset.y), 0).rgb;
    
    float3 ml = tex.SampleLevel(samp, uv + float2(-offset.x,       0.0), 0).rgb;
    float3 mc = tex.SampleLevel(samp, uv,                                0).rgb;
    float3 mr = tex.SampleLevel(samp, uv + float2( offset.x,       0.0), 0).rgb;
    
    float3 bl = tex.SampleLevel(samp, uv + float2(-offset.x,  offset.y), 0).rgb;
    float3 bc = tex.SampleLevel(samp, uv + float2(      0.0,  offset.y), 0).rgb;
    float3 br = tex.SampleLevel(samp, uv + float2( offset.x,  offset.y), 0).rgb;
    
    // Tent filter weights (1-2-1 pattern in each direction)
    // Total weight = 16
    //   1 2 1
    //   2 4 2
    //   1 2 1
    float3 result = mc * 4.0;
    result += (tc + ml + mr + bc) * 2.0;
    result += (tl + tr + bl + br) * 1.0;
    result /= 16.0;
    
    return result;
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    float2 texelSize = g_resolution.xy;
    
    // Upsample lower mip with tent filter
    float3 upsampled = TentFilter9Tap(g_lowerMipTex, g_linearSampler, texCoord, texelSize, g_filterRadius);
    
    // Sample higher resolution mip
    float3 higherMip = g_higherMipTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    
    // Additive blend (accumulate bloom from lower mips)
    // The blend factor controls how much of each mip level contributes
    float3 result = lerp(higherMip, upsampled, g_blendFactor) + upsampled;
    
    // Alternative: Simple additive blend
    // float3 result = higherMip + upsampled * g_blendFactor;
    
    return float4(result, 1.0);
}
