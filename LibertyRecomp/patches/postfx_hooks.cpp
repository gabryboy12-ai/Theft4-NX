#include "postfx_hooks.h"
#include <stdafx.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <cmath>
#include <cstring>

// =============================================================================
// PostFX Hooks Implementation
// =============================================================================
// Hooks into GTA IV's native post-processing system to:
// 1. Disable native effects when custom replacements are active
// 2. Extract sun direction and color data for custom sun shafts
// =============================================================================

namespace PostFXHooks
{
    // =========================================================================
    // Cached Base Pointer (set during Initialize)
    // =========================================================================
    static uint8_t* g_guestBase = nullptr;
    
    // =========================================================================
    // Saved Original Values (for restoration)
    // =========================================================================
    static float s_originalBloomIntensity = 1.0f;
    static float s_originalMotionBlurIntensity = 1.0f;
    static float s_originalDOFMultiplier = 1.0f;
    static bool s_originalValuesSaved = false;
    static bool s_nativeEffectsDisabled = false;
    
    // =========================================================================
    // Helper Functions - Big Endian Memory Access
    // Uses __builtin_bswap32 for efficiency (same pattern as lod_hooks.cpp)
    // =========================================================================
    
    // Read a big-endian float from PPC memory
    static inline float ReadFloatBE(uint8_t* base, uint32_t address)
    {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(base + address);
        uint32_t bits = __builtin_bswap32(*ptr);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    
    // Write a big-endian float to PPC memory
    static inline void WriteFloatBE(uint8_t* base, uint32_t address, float value)
    {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(base + address);
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        *ptr = __builtin_bswap32(bits);
    }
    
    // Read a big-endian uint32 from PPC memory
    static inline uint32_t ReadU32BE(uint8_t* base, uint32_t address)
    {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(base + address);
        return __builtin_bswap32(*ptr);
    }
    
    // =========================================================================
    // Initialization
    // =========================================================================
    
    void Init(uint8_t* base)
    {
        g_guestBase = base;
        s_originalValuesSaved = false;
        s_nativeEffectsDisabled = false;
        
        LOG_INFO("[PostFX] Initializing PostFX hooks system");
        LOGF_INFO("[PostFX] Guest base address: {:p}", (void*)base);
        LOGF_INFO("[PostFX] Bloom address: 0x{:08X}", Addresses::BLOOM_INTENSITY_CLAMP);
        LOGF_INFO("[PostFX] Motion blur address: 0x{:08X}", Addresses::MOTION_BLUR_INTENSITY);
        LOGF_INFO("[PostFX] Timecycle base: 0x{:08X}", Addresses::TIMECYCLE_DATA_BASE);
        LOGF_INFO("[PostFX] Light dir X/Y/Z: 0x{:08X}/0x{:08X}/0x{:08X}", 
                 Addresses::LIGHT_DIR_X, Addresses::LIGHT_DIR_Y, Addresses::LIGHT_DIR_Z);
        LOG_INFO("[PostFX] PostFX hooks initialized successfully");
    }
    
    // =========================================================================
    // Effect Control Functions
    // =========================================================================
    
    // Save original values before disabling (only once)
    static void SaveOriginalValues(uint8_t* base)
    {
        if (s_originalValuesSaved)
            return;
            
        s_originalBloomIntensity = ReadFloatBE(base, Addresses::BLOOM_INTENSITY_CLAMP);
        s_originalMotionBlurIntensity = ReadFloatBE(base, Addresses::MOTION_BLUR_INTENSITY);
        s_originalDOFMultiplier = ReadFloatBE(base, Addresses::DOF_MULTIPLIER);
        s_originalValuesSaved = true;
        
        LOG_INFO("[PostFX] Saved original values:");
        LOGF_INFO("[PostFX]   Bloom: {:.4f}", s_originalBloomIntensity);
        LOGF_INFO("[PostFX]   Motion blur: {:.4f}", s_originalMotionBlurIntensity);
        LOGF_INFO("[PostFX]   DOF: {:.4f}", s_originalDOFMultiplier);
    }
    
    void DisableNativeBloom(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return;
        SaveOriginalValues(base);
        WriteFloatBE(base, Addresses::BLOOM_INTENSITY_CLAMP, 0.0f);
    }
    
    void DisableNativeMotionBlur(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return;
        SaveOriginalValues(base);
        WriteFloatBE(base, Addresses::MOTION_BLUR_INTENSITY, 0.0f);
    }
    
    void DisableNativeDOF(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return;
        SaveOriginalValues(base);
        WriteFloatBE(base, Addresses::DOF_MULTIPLIER, 0.0f);
    }
    
    void DisableAllNativePostFX(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return;
        SaveOriginalValues(base);
        WriteFloatBE(base, Addresses::BLOOM_INTENSITY_CLAMP, 0.0f);
        WriteFloatBE(base, Addresses::MOTION_BLUR_INTENSITY, 0.0f);
        WriteFloatBE(base, Addresses::DOF_MULTIPLIER, 0.0f);
        s_nativeEffectsDisabled = true;
    }
    
    void RestoreNativePostFX(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return;
        if (!s_originalValuesSaved)
            return;
            
        WriteFloatBE(base, Addresses::BLOOM_INTENSITY_CLAMP, s_originalBloomIntensity);
        WriteFloatBE(base, Addresses::MOTION_BLUR_INTENSITY, s_originalMotionBlurIntensity);
        WriteFloatBE(base, Addresses::DOF_MULTIPLIER, s_originalDOFMultiplier);
        s_nativeEffectsDisabled = false;
        
        LOG_INFO("PostFXHooks: Restored native effect values");
    }
    
    // =========================================================================
    // Data Extraction Functions
    // =========================================================================
    
    bool ExtractSunDirection(uint8_t* base, float& outX, float& outY, float& outZ)
    {
        if (!base) base = g_guestBase;
        if (!base) {
            outX = 0.0f; outY = 1.0f; outZ = 0.0f;
            return false;
        }
        
        // Read directly from the light direction globals
        // These are set by sub_822B37A8 when updating shader parameters
        outX = ReadFloatBE(base, Addresses::LIGHT_DIR_X);
        outY = ReadFloatBE(base, Addresses::LIGHT_DIR_Y);
        outZ = ReadFloatBE(base, Addresses::LIGHT_DIR_Z);
        
        // Validate: direction should be normalized (length ~1.0)
        float length = std::sqrt(outX * outX + outY * outY + outZ * outZ);
        if (length < 0.1f || length > 2.0f)
        {
            // Invalid direction, use default (sun at noon)
            outX = 0.0f;
            outY = 1.0f;
            outZ = 0.0f;
            return false;
        }
        
        // Normalize if needed
        if (std::abs(length - 1.0f) > 0.01f)
        {
            outX /= length;
            outY /= length;
            outZ /= length;
        }
        
        return true;
    }
    
    bool ExtractSunDirectionFromTimecycle(uint8_t* base, float& outX, float& outY, float& outZ)
    {
        if (!base) base = g_guestBase;
        if (!base) {
            outX = 0.0f; outY = 1.0f; outZ = 0.0f;
            return false;
        }
        
        // Get current timecycle index
        uint32_t index = GetCurrentTimecycleIndex(base);
        
        // Calculate entry address: base + index * 128
        uint32_t entryAddr = Addresses::TIMECYCLE_DATA_BASE + (index * Addresses::TIMECYCLE_ENTRY_SIZE);
        
        // Read sun direction from timecycle entry
        outX = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_DIR_X_OFFSET);
        outY = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_DIR_Y_OFFSET);
        outZ = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_DIR_Z_OFFSET);
        
        // Validate
        float length = std::sqrt(outX * outX + outY * outY + outZ * outZ);
        if (length < 0.01f)
        {
            // Fallback to direct method
            return ExtractSunDirection(base, outX, outY, outZ);
        }
        
        // Normalize
        outX /= length;
        outY /= length;
        outZ /= length;
        
        return true;
    }
    
    bool ExtractSunColor(uint8_t* base, float& outR, float& outG, float& outB, float& outIntensity)
    {
        if (!base) base = g_guestBase;
        if (!base) {
            outR = 1.0f; outG = 0.95f; outB = 0.9f; outIntensity = 1.0f;
            return false;
        }
        
        // Get current timecycle index
        uint32_t index = GetCurrentTimecycleIndex(base);
        
        // Calculate entry address
        uint32_t entryAddr = Addresses::TIMECYCLE_DATA_BASE + (index * Addresses::TIMECYCLE_ENTRY_SIZE);
        
        // Read sun color from timecycle entry
        outR = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_COLOR_R_OFFSET);
        outG = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_COLOR_G_OFFSET);
        outB = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_COLOR_B_OFFSET);
        outIntensity = ReadFloatBE(base, entryAddr + Addresses::TC_SUN_INTENSITY_OFFSET);
        
        // Validate: colors should be non-negative
        if (outR < 0.0f || outG < 0.0f || outB < 0.0f)
        {
            // Default to warm sunlight
            outR = 1.0f;
            outG = 0.95f;
            outB = 0.9f;
            outIntensity = 1.0f;
            return false;
        }
        
        // Clamp intensity to reasonable range
        if (outIntensity < 0.0f) outIntensity = 0.0f;
        if (outIntensity > 10.0f) outIntensity = 10.0f;
        
        return true;
    }
    
    uint32_t GetCurrentTimecycleIndex(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) return 0;
        
        // Read from the primary timecycle index
        // Note: Game uses double-buffering, we read from index A
        uint32_t index = ReadU32BE(base, Addresses::TIMECYCLE_INDEX_A);
        
        // Validate index is within reasonable bounds
        // GTA IV typically has < 100 timecycle entries
        if (index > 1000)
        {
            // Fallback to index 0
            index = 0;
        }
        
        return index;
    }
    
    // =========================================================================
    // Screen Position Computation
    // =========================================================================
    
    void ComputeSunScreenPosition(const float* sunWorldDir,
                                  const float* viewProjMatrix,
                                  float* outScreenPos)
    {
        // Sun is at "infinity" in the given direction
        // We use a point far in that direction for projection
        // Position = camera position + sunDir * very_large_distance
        // Since we only care about direction, use sunDir as homogeneous point with w=0
        
        // Transform direction by view-projection matrix (treating as direction, not point)
        // For a direction vector, we want to project the point at infinity
        // clipPos = viewProj * (sunDir, 0) -- but that gives the vanishing point
        // Better: use (sunDir * farDist, 1) and project
        
        const float farDist = 10000.0f;
        float point[4] = {
            sunWorldDir[0] * farDist,
            sunWorldDir[1] * farDist,
            sunWorldDir[2] * farDist,
            1.0f
        };
        
        // Matrix multiply (row-major): clipPos = viewProjMatrix * point
        float clipPos[4];
        for (int i = 0; i < 4; i++)
        {
            clipPos[i] = viewProjMatrix[i * 4 + 0] * point[0] +
                         viewProjMatrix[i * 4 + 1] * point[1] +
                         viewProjMatrix[i * 4 + 2] * point[2] +
                         viewProjMatrix[i * 4 + 3] * point[3];
        }
        
        // Perspective divide
        if (std::abs(clipPos[3]) > 0.0001f)
        {
            float invW = 1.0f / clipPos[3];
            float ndcX = clipPos[0] * invW;
            float ndcY = clipPos[1] * invW;
            float ndcZ = clipPos[2] * invW;
            
            // Convert NDC [-1,1] to UV [0,1]
            outScreenPos[0] = ndcX * 0.5f + 0.5f;
            outScreenPos[1] = 1.0f - (ndcY * 0.5f + 0.5f);  // Flip Y for screen coords
            outScreenPos[2] = ndcZ;  // Depth (positive = in front)
        }
        else
        {
            // Degenerate case
            outScreenPos[0] = 0.5f;
            outScreenPos[1] = 0.5f;
            outScreenPos[2] = -1.0f;  // Behind camera
        }
        
        // Check if sun is behind camera (w < 0 means behind in some conventions)
        // Also check if NDC is outside frustum
        if (clipPos[3] < 0.0f)
        {
            outScreenPos[2] = -1.0f;  // Mark as behind camera
        }
    }
    
    // =========================================================================
    // Debug Functions
    // =========================================================================
    
    void DebugPrintState(uint8_t* base)
    {
        if (!base) base = g_guestBase;
        if (!base) {
            LOG_WARNING("[PostFX] DebugPrintState: base address not set");
            return;
        }
        
        LOG_INFO("=== PostFX State ===");
        
        // Effect multipliers
        float bloom = ReadFloatBE(base, Addresses::BLOOM_INTENSITY_CLAMP);
        float motionBlur = ReadFloatBE(base, Addresses::MOTION_BLUR_INTENSITY);
        float dof = ReadFloatBE(base, Addresses::DOF_MULTIPLIER);
        
        LOG_INFO("Effect Multipliers:");
        LOGF_INFO("  Bloom intensity: {:.4f}", bloom);
        LOGF_INFO("  Motion blur: {:.4f}", motionBlur);
        LOGF_INFO("  DOF: {:.4f}", dof);
        
        // Sun direction
        float sunX, sunY, sunZ;
        if (ExtractSunDirection(base, sunX, sunY, sunZ))
        {
            LOGF_INFO("Sun Direction (direct): ({:.4f}, {:.4f}, {:.4f})", sunX, sunY, sunZ);
        }
        else
        {
            LOG_WARNING("Sun Direction: Failed to extract");
        }
        
        // Sun from timecycle
        if (ExtractSunDirectionFromTimecycle(base, sunX, sunY, sunZ))
        {
            LOGF_INFO("Sun Direction (timecycle): ({:.4f}, {:.4f}, {:.4f})", sunX, sunY, sunZ);
        }
        
        // Sun color
        float r, g, b, intensity;
        if (ExtractSunColor(base, r, g, b, intensity))
        {
            LOGF_INFO("Sun Color: ({:.4f}, {:.4f}, {:.4f}) x {:.4f}", r, g, b, intensity);
        }
        
        // Timecycle index
        uint32_t tcIndex = GetCurrentTimecycleIndex(base);
        LOGF_INFO("Timecycle Index: {}", tcIndex);
        
        LOG_INFO("====================");
    }
    
} // namespace PostFXHooks
