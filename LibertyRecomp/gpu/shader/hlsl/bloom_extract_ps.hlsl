// Bloom Extract (Bright Pass) Pixel Shader
// Extracts pixels above a luminance threshold for bloom processing
// Uses Karis average to prevent fireflies (bright single pixels causing bloom spikes)
// 
// Reference: MiniEngine by Microsoft, "Next Generation Post Processing" by Jorge Jimenez

Texture2D g_colorTex : register(t0);
SamplerState g_linearSampler : register(s0);

cbuffer BloomExtractConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float g_threshold;        // Brightness threshold (0.8 - 1.5 typical)
    float g_softThreshold;    // Soft knee for smoother threshold transition (0.0 - 0.5)
    float g_intensity;        // Bloom intensity multiplier
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Rec. 709 luminance (sRGB primaries)
float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Soft thresholding with knee curve
// Allows gradual contribution near threshold instead of hard cutoff
float3 SoftThreshold(float3 color, float threshold, float softKnee)
{
    float luma = Luminance(color);
    
    // Knee curve parameters
    float knee = threshold * softKnee;
    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    
    // Contribution factor
    float contribution = max(soft, luma - threshold) / max(luma, 0.00001);
    
    return color * contribution;
}

// Karis average - weighted average that reduces fireflies
// Weights bright pixels less to prevent them from dominating
float3 KarisAverage(float3 c1, float3 c2, float3 c3, float3 c4)
{
    const float kSmallEpsilon = 0.0001;
    
    float luma1 = Luminance(c1);
    float luma2 = Luminance(c2);
    float luma3 = Luminance(c3);
    float luma4 = Luminance(c4);
    
    // Weight inversely by luminance + 1 (Karis weight)
    float w1 = 1.0 / (1.0 + luma1);
    float w2 = 1.0 / (1.0 + luma2);
    float w3 = 1.0 / (1.0 + luma3);
    float w4 = 1.0 / (1.0 + luma4);
    
    float wSum = w1 + w2 + w3 + w4;
    
    return (c1 * w1 + c2 * w2 + c3 * w3 + c4 * w4) / wSum;
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    float2 texelSize = g_resolution.xy;
    
    // Sample in a 2x2 pattern with bilinear filtering for initial downsample
    // This is the first step of the bloom pyramid (typically 1/2 res)
    float2 halfTexel = texelSize * 0.5;
    
    float3 c1 = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-halfTexel.x, -halfTexel.y), 0).rgb;
    float3 c2 = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( halfTexel.x, -halfTexel.y), 0).rgb;
    float3 c3 = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2(-halfTexel.x,  halfTexel.y), 0).rgb;
    float3 c4 = g_colorTex.SampleLevel(g_linearSampler, texCoord + float2( halfTexel.x,  halfTexel.y), 0).rgb;
    
    // Apply threshold to each sample
    c1 = SoftThreshold(c1, g_threshold, g_softThreshold);
    c2 = SoftThreshold(c2, g_threshold, g_softThreshold);
    c3 = SoftThreshold(c3, g_threshold, g_softThreshold);
    c4 = SoftThreshold(c4, g_threshold, g_softThreshold);
    
    // Use Karis average to reduce fireflies
    float3 bloom = KarisAverage(c1, c2, c3, c4);
    
    // Apply intensity
    bloom *= g_intensity;
    
    return float4(bloom, 1.0);
}
