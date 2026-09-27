/**
 * XEX Validator for Xbox 360 executable files
 * Validates game version, region, and title ID
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

namespace liberty {
namespace install {

// Xbox 360 Region flags (from XEX header)
enum class XexRegion : uint32_t {
    NTSCU       = 0x000000FF,   // North America
    NTSCJ       = 0x0000FF00,   // Japan
    NTSCJ_Japan = 0x00000100,
    NTSCJ_China = 0x00000200,
    PAL         = 0x00FF0000,   // Europe/Australia
    PAL_AU_NZ   = 0x00010000,
    Other       = 0xFF000000,
    All         = 0xFFFFFFFF,   // Region-free
};

// Validation result
struct XexValidationResult {
    bool isValid = false;
    bool isCorrectGame = false;
    bool isCorrectRegion = false;
    
    uint32_t titleId = 0;
    uint32_t mediaId = 0;
    uint32_t region = 0;
    uint32_t version = 0;
    uint8_t discNumber = 0;
    uint8_t discCount = 0;
    
    std::string errorMessage;
    std::string detectedRegionName;
    std::string requiredRegionName;
};

// GTA IV specific constants
namespace GTA4 {
    // GTA IV Title ID (same for all regions)
    static constexpr uint32_t TitleId = 0x545407F2;
    
    // Media IDs for DLC detection
    static constexpr uint32_t MediaId_TLAD = 0x00000002;
    static constexpr uint32_t MediaId_TBOGT = 0x00000003;
    
    // Expected region for US version
    static constexpr uint32_t RequiredRegion = static_cast<uint32_t>(XexRegion::NTSCU);
}

class XexValidator {
public:
    XexValidator() = default;
    ~XexValidator() = default;

    // Validate XEX file from path
    XexValidationResult validate(const std::filesystem::path& xexPath);
    
    // Validate XEX data from memory
    XexValidationResult validate(const uint8_t* data, size_t size);
    
    // Get human-readable region name
    static std::string getRegionName(uint32_t region);
    
    // Check if region matches expected
    static bool isRegionCompatible(uint32_t xexRegion, uint32_t requiredRegion);

private:
    // Parse XEX execution info header
    bool parseExecutionInfo(const uint8_t* data, size_t size, XexValidationResult& result);
    
    // Parse XEX security info
    bool parseSecurityInfo(const uint8_t* data, size_t size, XexValidationResult& result);
};

} // namespace install
} // namespace liberty
