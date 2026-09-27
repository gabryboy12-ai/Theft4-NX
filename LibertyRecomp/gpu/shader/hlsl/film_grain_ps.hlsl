// Film Grain Pixel Shader
// Adds cinematic film grain noise to the image
// 
// Based on Jorge Jimenez's Interleaved Gradient Noise (IGN) technique from
// "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014)
// 
// Key features:
// - Uses IGN for TAA-compatible noise (avoids temporal crawling artifacts)
// - Resolution-independent grain sizing
// - Physically-inspired luminance response (film characteristic curve)
// - Temporal animation using proper IGN scrolling formula

Texture2D g_colorTex : register(t0);
SamplerState g_linearSampler : register(s0);

cbuffer FilmGrainConstants : register(b0)
{
    float4 g_resolution;     // (1/width, 1/height, width, height)
    float g_intensity;       // Grain intensity (0.0-1.0)
    float g_frameIndex;      // Frame index for temporal animation (use instead of time)
    float g_luminanceScale;  // How much grain affects bright vs dark areas
    float g_coloredGrain;    // 0 = monochrome, 1 = colored grain
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// =============================================================================
// Interleaved Gradient Noise (IGN) - Jorge Jimenez, Activision
// =============================================================================
// This noise pattern is specifically designed to work well with TAA's 3x3
// neighborhood clamping. Every 3x3 block of pixels contains values that are
// well-distributed across the 0-1 range, making TAA's history rejection more
// accurate and reducing temporal artifacts.
//
// Reference: http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
// Additional analysis: https://blog.demofox.org/2022/01/01/interleaved-gradient-noise-a-different-kind-of-low-discrepancy-sequence/
// =============================================================================

// Static IGN - for spatial noise pattern
float InterleavedGradientNoise(float2 pixelCoord)
{
    // Magic constants found by Jorge Jimenez through extensive manual tuning
    // These ensure low discrepancy within every 3x3 pixel neighborhood
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixelCoord, magic.xy)));
}

// Animated IGN - scrolls 5.588238 pixels per frame for temporal low-discrepancy
// This offset was found by Jorge Jimenez to maintain IGN properties over time
float InterleavedGradientNoiseAnimated(float2 pixelCoord, float frameIndex)
{
    // Periodically reset frame to avoid floating point precision issues
    frameIndex = fmod(frameIndex, 64.0);
    
    // Scroll the noise pattern - 5.588238 is the magic temporal offset
    float2 animatedCoord = pixelCoord + 5.588238 * frameIndex;
    
    return InterleavedGradientNoise(animatedCoord);
}

// =============================================================================
// Film Characteristic Curve - Luminance Response
// =============================================================================
// Real film grain is most visible in midtones and fades in deep shadows and
// bright highlights. This models the photographic response curve where:
// - Shadows: grain is crushed/hidden in toe region
// - Midtones: maximum grain visibility  
// - Highlights: grain is blown out/clipped in shoulder region
// =============================================================================
float FilmGrainResponse(float luminance, float luminanceScale)
{
    // Smooth S-curve response that peaks at midtones
    // Using smoothstep for natural falloff at both ends
    float shadowFalloff = smoothstep(0.0, 0.3, luminance);
    float highlightFalloff = smoothstep(1.0, 0.7, luminance);
    
    // Combine with adjustable scale
    float response = shadowFalloff * highlightFalloff;
    
    // Allow user control over how much the curve affects grain
    return lerp(1.0, response, luminanceScale);
}

// =============================================================================
// Multi-Octave IGN for Film-Like Grain Texture
// =============================================================================
// Real film grain has a characteristic texture from overlapping silver halide
// crystals of varying sizes. We approximate this with multi-frequency noise.
// =============================================================================
float FilmGrainNoise(float2 pixelCoord, float frameIndex)
{
    // Base grain layer (primary crystal size)
    float grain = InterleavedGradientNoiseAnimated(pixelCoord, frameIndex) * 2.0 - 1.0;
    
    // Add finer detail layers (smaller crystals) with decreasing amplitude
    // Offset coordinates to decorrelate layers
    float grain2 = InterleavedGradientNoiseAnimated(pixelCoord * 2.0 + float2(127.1, 311.7), frameIndex);
    float grain3 = InterleavedGradientNoiseAnimated(pixelCoord * 4.0 + float2(269.5, 183.3), frameIndex);
    
    grain += (grain2 * 2.0 - 1.0) * 0.5;
    grain += (grain3 * 2.0 - 1.0) * 0.25;
    
    // Normalize to roughly -1 to 1 range
    grain *= 0.571; // 1.0 / (1.0 + 0.5 + 0.25)
    
    return grain;
}

// Colored grain with per-channel IGN for chromatic variation
float3 FilmGrainNoiseColored(float2 pixelCoord, float frameIndex)
{
    float3 grain;
    
    // Each color channel uses offset coordinates for decorrelation
    // This simulates how different dye layers in color film have independent grain
    grain.r = FilmGrainNoise(pixelCoord, frameIndex);
    grain.g = FilmGrainNoise(pixelCoord + float2(97.0, 43.0), frameIndex + 17.0);
    grain.b = FilmGrainNoise(pixelCoord + float2(43.0, 97.0), frameIndex + 31.0);
    
    return grain;
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    
    // Sample original color
    float4 color = g_colorTex.SampleLevel(g_linearSampler, texCoord, 0);
    
    // Calculate luminance using Rec. 709 coefficients (matches sRGB)
    float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    
    // Apply film characteristic curve for realistic grain response
    float grainResponse = FilmGrainResponse(luminance, g_luminanceScale);
    
    // Use pixel coordinates for resolution-independent grain size
    // The grain pattern will look consistent regardless of resolution
    float2 pixelCoord = input.position.xy;
    
    // Scale intensity by luminance response
    float scaledIntensity = g_intensity * grainResponse;
    
    // Generate grain using appropriate mode
    float3 grain;
    
    if (g_coloredGrain > 0.5)
    {
        // Full colored grain mode - chromatic grain like color negative film
        grain = FilmGrainNoiseColored(pixelCoord, g_frameIndex) * scaledIntensity;
    }
    else if (g_coloredGrain > 0.0)
    {
        // Blend between mono and colored
        float3 coloredGrain = FilmGrainNoiseColored(pixelCoord, g_frameIndex);
        float monoGrain = FilmGrainNoise(pixelCoord, g_frameIndex);
        grain = lerp(float3(monoGrain, monoGrain, monoGrain), coloredGrain, g_coloredGrain) * scaledIntensity;
    }
    else
    {
        // Pure monochrome grain mode - like black and white film
        float monoGrain = FilmGrainNoise(pixelCoord, g_frameIndex);
        grain = float3(monoGrain, monoGrain, monoGrain) * scaledIntensity;
    }
    
    // Apply grain additively
    // Note: This should be applied AFTER TAA to avoid noise reduction fighting the grain
    color.rgb += grain;
    
    // Clamp to valid range to prevent negative values
    color.rgb = max(color.rgb, 0.0);
    
    return color;
}
