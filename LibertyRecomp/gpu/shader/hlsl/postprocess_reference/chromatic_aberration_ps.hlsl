// Chromatic Aberration Pixel Shader
// Simulates lens color fringing by offsetting RGB channels
// Supports both lateral (transverse) and configurable spectral offsets

Texture2D g_colorTex : register(t0);
SamplerState g_linearSampler : register(s0);

cbuffer ChromaticAberrationConstants : register(b0)
{
    float4 g_resolution;      // (1/width, 1/height, width, height)
    float g_intensity;        // Overall intensity multiplier
    float g_redOffset;        // Red channel offset multiplier (positive = outward)
    float g_greenOffset;      // Green channel offset multiplier (usually small/zero)
    float g_blueOffset;       // Blue channel offset multiplier (positive = outward)
    float g_radialFalloff;    // How much effect increases toward edges (1.0 = linear, 2.0 = quadratic)
    float2 g_center;          // Custom center point for aberration (default 0.5, 0.5)
    float g_aspectCorrection; // Aspect ratio correction (width/height, or 1.0 to disable)
    float g_maxOffset;        // Maximum offset in pixels (prevents extreme distortion)
    float g_softKnee;         // Falloff smoothness (0 = sharp, 1 = smooth)
    float2 g_padding;         // Alignment padding
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Clamp UV to prevent sampling outside texture bounds (avoids edge artifacts)
float2 ClampUV(float2 uv, float2 pixelSize)
{
    float2 halfPixel = pixelSize * 0.5;
    return clamp(uv, halfPixel, 1.0 - halfPixel);
}

float4 shaderMain(PSInput input) : SV_Target
{
    float2 texCoord = input.texCoord;
    float2 pixelSize = g_resolution.xy;
    
    // Calculate offset from configurable center point
    float2 centerOffset = texCoord - g_center;
    
    // Apply aspect ratio correction to make aberration circular on non-square displays
    float2 correctedOffset = centerOffset;
    correctedOffset.x *= g_aspectCorrection;
    
    float distFromCenter = length(correctedOffset);
    
    // Handle exact center pixel - no aberration needed
    if (distFromCenter < 0.0001)
    {
        return g_colorTex.SampleLevel(g_linearSampler, texCoord, 0);
    }
    
    // Smooth falloff using soft knee for more natural transition
    // Combines smoothstep with power curve for artistic control
    float normalizedDist = saturate(distFromCenter * 2.0);
    float smoothDist = lerp(normalizedDist, smoothstep(0.0, 1.0, normalizedDist), g_softKnee);
    float falloff = pow(smoothDist, g_radialFalloff);
    
    // Direction from center (for radial aberration)
    // Use uncorrected offset for direction to maintain proper radial pattern
    float2 direction = centerOffset / length(centerOffset);
    
    // Calculate per-channel offsets with intensity and falloff
    float offsetScale = g_intensity * falloff;
    
    // Compute raw offsets
    float2 redOffsetRaw = direction * pixelSize * g_redOffset * offsetScale;
    float2 greenOffsetRaw = direction * pixelSize * g_greenOffset * offsetScale;
    float2 blueOffsetRaw = direction * pixelSize * g_blueOffset * offsetScale;
    
    // Clamp maximum offset to prevent extreme distortion
    float maxOffsetPixels = g_maxOffset * length(pixelSize);
    float redMag = length(redOffsetRaw);
    float greenMag = length(greenOffsetRaw);
    float blueMag = length(blueOffsetRaw);
    
    float2 redOffset = redMag > maxOffsetPixels ? redOffsetRaw * (maxOffsetPixels / redMag) : redOffsetRaw;
    float2 greenOffset = greenMag > maxOffsetPixels ? greenOffsetRaw * (maxOffsetPixels / greenMag) : greenOffsetRaw;
    float2 blueOffset = blueMag > maxOffsetPixels ? blueOffsetRaw * (maxOffsetPixels / blueMag) : blueOffsetRaw;
    
    // Sample each channel with offset and edge clamping
    // Red typically shifts outward, blue inward for realistic lens CA
    // Green can have slight offset for more accurate spectral simulation
    float2 redUV = ClampUV(texCoord + redOffset, pixelSize);
    float2 greenUV = ClampUV(texCoord + greenOffset, pixelSize);
    float2 blueUV = ClampUV(texCoord - blueOffset, pixelSize);
    
    float r = g_colorTex.SampleLevel(g_linearSampler, redUV, 0).r;
    
    // Optimization: Sample green and alpha together (saves one texture fetch)
    float4 centerSample = g_colorTex.SampleLevel(g_linearSampler, greenUV, 0);
    float g = centerSample.g;
    float a = centerSample.a;
    
    float b = g_colorTex.SampleLevel(g_linearSampler, blueUV, 0).b;
    
    return float4(r, g, b, a);
}
