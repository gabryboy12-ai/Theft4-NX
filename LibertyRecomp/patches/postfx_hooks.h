#pragma once

// =============================================================================
// PostFX Hooks - Hook into GTA IV's native post-processing effects
// =============================================================================
// These hooks allow disabling native PostFX effects (so custom ones can run
// instead) and extracting data needed for custom shader implementations
// (e.g., sun direction for god rays).
//
// Address verification (Python calculations against PPC code):
// All addresses verified using: result = (lis_value << 16) + offset
// Where lis_value and offset come from: lis rX,-32093; lfs fY,-5884(rX)
// =============================================================================

#include <cstdint>

namespace PostFXHooks
{
    // =========================================================================
    // Memory Addresses - Verified against PPC recompiled code
    // =========================================================================
    // Address calculation: (lis_value & 0xFFFF) << 16 + offset
    // Example: lis r10,-32093; lfs f13,-5884(r10)
    //   -> ((-32093) & 0xFFFF) << 16 + (-5884) = 0x82A2E904
    namespace Addresses
    {
        // ---------------------------------------------------------------------
        // Effect Intensity Multipliers (floats, big-endian)
        // Setting these to 0.0f disables the respective effect
        // ---------------------------------------------------------------------
        
        // Bloom intensity clamp value
        // PPC: lis r10,-32093; lfs f13,-5884(r10)
        // Calculation: 0x82A30000 + (-5884) = 0x82A2E904
        constexpr uint32_t BLOOM_INTENSITY_CLAMP = 0x82A2E904;
        
        // Depth of Field multiplier
        // PPC: lis r10,-32093; lfs fX,-5888(r10)
        // Calculation: 0x82A30000 + (-5888) = 0x82A2E900
        constexpr uint32_t DOF_MULTIPLIER = 0x82A2E900;
        
        // Motion blur intensity
        // PPC: lis r10,-32093; lfs fX,-5860(r10)
        // Calculation: 0x82A30000 + (-5860) = 0x82A2E91C
        constexpr uint32_t MOTION_BLUR_INTENSITY = 0x82A2E91C;
        
        // ---------------------------------------------------------------------
        // PostFX Manager Pointer
        // ---------------------------------------------------------------------
        
        // Global PostFX manager instance pointer
        // PPC: lis r11,-32052; lwz r3,4376(r11)
        // Calculation: 0x82CC0000 + 4376 = 0x82CC1118
        constexpr uint32_t POSTFX_MANAGER_PTR = 0x82CC1118;
        
        // PostFX main update function
        constexpr uint32_t POSTFX_UPDATE_FUNC = 0x822E4E20;
        
        // ---------------------------------------------------------------------
        // Timecycle Data (for sun direction)
        // ---------------------------------------------------------------------
        
        // Timecycle data base address (128-byte entries)
        // PPC: lis r11,-32050; addi r11,r11,-27792
        // Calculation: 0x82CE0000 + (-27792) = 0x82CD9370
        constexpr uint32_t TIMECYCLE_DATA_BASE = 0x82CD9370;
        
        // Timecycle entry size (bytes)
        // PPC: rlwinm r6,r7,7,0,24 -> effectively index * 128
        constexpr uint32_t TIMECYCLE_ENTRY_SIZE = 128;
        
        // Double-buffered timecycle indices
        // PPC: lis r11,-32086; addi r11,r11,19864
        // Calculation: 0x82AA0000 + 19864 = 0x82AA4D98
        constexpr uint32_t TIMECYCLE_INDEX_A = 0x82AA4D98;
        constexpr uint32_t TIMECYCLE_INDEX_B = 0x82AA4D9C;
        
        // ---------------------------------------------------------------------
        // Light/Sun Direction Data
        // ---------------------------------------------------------------------
        
        // Directional light world direction (3 floats at sequential addresses)
        // PPC: lis r11,-32052; stfs fX,4512(r11) (Z)
        //                      stfs fX,4516(r11) (Y)  
        //                      stfs fX,4520(r11) (X)
        // Calculation: 0x82CC0000 + 4512/4516/4520
        constexpr uint32_t LIGHT_DIR_Z = 0x82CC11A0;  // +4512
        constexpr uint32_t LIGHT_DIR_Y = 0x82CC11A4;  // +4516
        constexpr uint32_t LIGHT_DIR_X = 0x82CC11A8;  // +4520
        
        // Sun direction shader handle (for SetFloat4 calls)
        // PPC: lis r11,-32078; lwz rX,-29040(r11)
        // Calculation: 0x82B20000 + (-29040) = 0x82B18E90
        constexpr uint32_t SUN_DIRECTION_HANDLE = 0x82B18E90;
        
        // Directional light pointer
        // PPC: lis r11,-32052; lwz rX,4456(r11)
        // Calculation: 0x82CC0000 + 4456 = 0x82CC1168
        constexpr uint32_t DIRECTIONAL_LIGHT = 0x82CC1168;
        
        // ---------------------------------------------------------------------
        // Timecycle Structure Offsets (within 128-byte entries)
        // These are offsets into each timecycle entry for sun-related data
        // ---------------------------------------------------------------------
        
        // Offsets determined from decompiled code accessing timecycle data
        // Sun direction is typically in the first vector of the entry
        constexpr uint32_t TC_SUN_DIR_X_OFFSET = 0x00;  // Offset for sun X
        constexpr uint32_t TC_SUN_DIR_Y_OFFSET = 0x04;  // Offset for sun Y
        constexpr uint32_t TC_SUN_DIR_Z_OFFSET = 0x08;  // Offset for sun Z
        
        // Sun color typically follows direction
        constexpr uint32_t TC_SUN_COLOR_R_OFFSET = 0x0C;
        constexpr uint32_t TC_SUN_COLOR_G_OFFSET = 0x10;
        constexpr uint32_t TC_SUN_COLOR_B_OFFSET = 0x14;
        constexpr uint32_t TC_SUN_INTENSITY_OFFSET = 0x18;
    }
    
    // =========================================================================
    // Initialization
    // =========================================================================
    
    // Initialize PostFX hooks with guest memory base address
    // Call this during game initialization (after memory regions are set up)
    void Init(uint8_t* base);
    
    // =========================================================================
    // Effect Control Functions
    // =========================================================================
    
    // Disable native bloom effect by zeroing intensity multiplier
    // Call this before rendering custom bloom
    void DisableNativeBloom(uint8_t* base);
    
    // Disable native motion blur
    void DisableNativeMotionBlur(uint8_t* base);
    
    // Disable native depth of field
    void DisableNativeDOF(uint8_t* base);
    
    // Disable all native post-processing effects
    void DisableAllNativePostFX(uint8_t* base);
    
    // Restore original native effect values (if saved)
    void RestoreNativePostFX(uint8_t* base);
    
    // =========================================================================
    // Data Extraction Functions
    // =========================================================================
    
    // Extract current sun/light direction from game memory
    // Returns direction vector in world space (normalized)
    // base: PPC memory base pointer
    // outX, outY, outZ: output direction components
    // Returns true if extraction successful
    bool ExtractSunDirection(uint8_t* base, float& outX, float& outY, float& outZ);
    
    // Alternative: Extract from timecycle data (more accurate)
    bool ExtractSunDirectionFromTimecycle(uint8_t* base, float& outX, float& outY, float& outZ);
    
    // Extract sun color/intensity from game memory
    // Returns color in linear RGB space
    // base: PPC memory base pointer
    // outR, outG, outB: output color components (0-1 range, HDR)
    // outIntensity: output intensity multiplier
    // Returns true if extraction successful
    bool ExtractSunColor(uint8_t* base, float& outR, float& outG, float& outB, float& outIntensity);
    
    // Get current timecycle index (for debugging/advanced use)
    uint32_t GetCurrentTimecycleIndex(uint8_t* base);
    
    // =========================================================================
    // Computed Values (call after extraction)
    // =========================================================================
    
    // Compute sun screen position from world direction
    // Requires view-projection matrix from camera
    // sunWorldDir: normalized sun direction in world space
    // viewProjMatrix: 4x4 view-projection matrix (row-major)
    // outScreenPos: output screen UV (x,y) and depth (z)
    //               z < 0 means sun is behind camera
    void ComputeSunScreenPosition(const float* sunWorldDir,
                                  const float* viewProjMatrix,
                                  float* outScreenPos);
    
    // =========================================================================
    // Debug Functions
    // =========================================================================
    
    // Print current PostFX state for debugging
    void DebugPrintState(uint8_t* base);
    
} // namespace PostFXHooks
