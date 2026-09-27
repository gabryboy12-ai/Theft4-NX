// Sun Shafts (God Rays) Radial Blur Pixel Shader
// Creates volumetric light scattering effect by radial blur toward sun position
//
// Reference: GPU Gems 3 - Chapter 13 "Volumetric Light Scattering as a Post-Process"
// Original algorithm by Kenny Mitchell, Electronic Arts
// Ported from FusionFix SunShafts_PS.hlsl

Texture2D g_prepassTex : register(t0);    // Prepass result (sun/sky extraction)
SamplerState g_linearSampler : register(s0);

cbuffer SunShaftsRadialConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float2 g_sunScreenPos;    // Sun position in screen space UV coordinates
    float g_density;          // Ray density - how long the rays are (0.5 - 1.0)
    float g_decay;            // How fast rays fade from center (0.9 - 0.99)
    float g_exposure;         // Exposure/brightness of rays (0.1 - 1.0)
    float g_weight;           // Weight for each sample (0.5 - 1.0)
    int g_numSamples;         // Number of ray march samples (16-64)
    float g_padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    
    // Calculate vector from pixel to sun in screen space
    float2 deltaTexCoord = texCoord - g_sunScreenPos;
    
    // Scale by density and number of samples
    deltaTexCoord *= (1.0 / float(g_numSamples)) * g_density;
    
    // Start with current sample
    float3 color = g_prepassTex.SampleLevel(g_linearSampler, texCoord, 0).rgb;
    
    // Initialize illumination decay factor
    float illuminationDecay = 1.0;
    
    // Current sample position
    float2 samplePos = texCoord;
    
    // Accumulate samples along ray toward sun
    [loop]
    for (int i = 0; i < g_numSamples; i++)
    {
        // Step sample location toward sun
        samplePos -= deltaTexCoord;
        
        // Clamp to valid UV range
        samplePos = saturate(samplePos);
        
        // Sample at new location
        float3 sampleColor = g_prepassTex.SampleLevel(g_linearSampler, samplePos, 0).rgb;
        
        // Apply decay and weight
        sampleColor *= illuminationDecay * g_weight;
        
        // Accumulate
        color += sampleColor;
        
        // Update exponential decay factor
        illuminationDecay *= g_decay;
    }
    
    // Apply final exposure
    color *= g_exposure;
    
    return float4(color, 1.0);
}
