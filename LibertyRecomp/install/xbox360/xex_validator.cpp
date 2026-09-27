/**
 * XEX Validator implementation
 * Parses Xbox 360 XEX headers to validate game version and region
 */

#include "xex_validator.h"
#include <fstream>
#include <cstring>

namespace liberty {
namespace install {

// XEX2 magic bytes
static constexpr uint32_t XEX2_MAGIC = 0x58455832;  // "XEX2"

// XEX header key for execution info
static constexpr uint32_t XEX_HEADER_EXECUTION_INFO = 0x00040006;

// Big-endian to host conversion
static inline uint32_t be32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8)  | uint32_t(data[3]);
}

static inline uint16_t be16(const uint8_t* data) {
    return (uint16_t(data[0]) << 8) | uint16_t(data[1]);
}

XexValidationResult XexValidator::validate(const std::filesystem::path& xexPath) {
    XexValidationResult result;
    
    std::ifstream file(xexPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.errorMessage = "Could not open XEX file";
        return result;
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0);
    
    // Read at least enough for headers (usually ~64KB is plenty)
    size_t readSize = std::min(fileSize, size_t(256 * 1024));
    std::vector<uint8_t> data(readSize);
    file.read(reinterpret_cast<char*>(data.data()), readSize);
    
    return validate(data.data(), data.size());
}

XexValidationResult XexValidator::validate(const uint8_t* data, size_t size) {
    XexValidationResult result;
    
    if (size < 24) {
        result.errorMessage = "File too small to be a valid XEX";
        return result;
    }
    
    // Check magic
    uint32_t magic = be32(data);
    if (magic != XEX2_MAGIC) {
        result.errorMessage = "Not a valid XEX2 file (invalid magic)";
        return result;
    }
    
    // Parse headers
    if (!parseSecurityInfo(data, size, result)) {
        return result;
    }
    
    if (!parseExecutionInfo(data, size, result)) {
        return result;
    }
    
    result.isValid = true;
    
    // Check if this is GTA IV
    result.isCorrectGame = (result.titleId == GTA4::TitleId);
    
    // Check region
    result.isCorrectRegion = isRegionCompatible(result.region, GTA4::RequiredRegion);
    result.detectedRegionName = getRegionName(result.region);
    result.requiredRegionName = getRegionName(GTA4::RequiredRegion);
    
    // Generate error message if needed
    if (!result.isCorrectGame) {
        result.errorMessage = "This is not GTA IV (wrong Title ID)";
    } else if (!result.isCorrectRegion) {
        result.errorMessage = "This ROM is the correct game, but the wrong version.\n"
                              "This project requires the NTSC-U (USA) version of the game.";
    }
    
    return result;
}

bool XexValidator::parseSecurityInfo(const uint8_t* data, size_t size, XexValidationResult& result) {
    if (size < 24) {
        result.errorMessage = "XEX too small for security info";
        return false;
    }
    
    // XEX2 header structure:
    // 0x00: magic (4)
    // 0x04: module flags (4)
    // 0x08: header size (4)
    // 0x0C: reserved (4)
    // 0x10: security offset (4)
    // 0x14: header count (4)
    
    uint32_t securityOffset = be32(data + 0x10);
    
    // Security info needs at least 0x180 bytes
    if (securityOffset + 0x180 > size) {
        result.errorMessage = "Security info extends beyond file";
        return false;
    }
    
    const uint8_t* secInfo = data + securityOffset;
    
    // xex2_security_info structure (from Xenia):
    // 0x000: header_size (4)
    // 0x004: image_size (4)
    // 0x008: rsa_signature (0x100)
    // 0x108: unk (4)
    // 0x10C: image_flags (4)
    // 0x110: load_address (4)
    // 0x114: section_digest (0x14)
    // 0x128: import_table_count (4)
    // 0x12C: import_table_digest (0x14)
    // 0x140: xgd2_media_id (0x10)
    // 0x150: aes_key (0x10)
    // 0x160: export_table (4)
    // 0x164: header_digest (0x14)
    // 0x178: region (4)           <-- Region flags!
    // 0x17C: allowed_media_types (4)
    // 0x180: page_descriptor_count (4)
    
    result.region = be32(secInfo + 0x178);
    
    return true;
}

bool XexValidator::parseExecutionInfo(const uint8_t* data, size_t size, XexValidationResult& result) {
    if (size < 24) {
        result.errorMessage = "XEX too small for header parsing";
        return false;
    }
    
    uint32_t headerCount = be32(data + 0x14);
    
    // Optional headers start at offset 0x18
    const uint8_t* optHeaders = data + 0x18;
    
    for (uint32_t i = 0; i < headerCount; i++) {
        size_t headerOffset = i * 8;
        if (0x18 + headerOffset + 8 > size) {
            break;
        }
        
        uint32_t key = be32(optHeaders + headerOffset);
        uint32_t value = be32(optHeaders + headerOffset + 4);
        
        if (key == XEX_HEADER_EXECUTION_INFO) {
            // Execution info is at the offset specified by value
            if (value + 0x18 > size) {
                result.errorMessage = "Execution info extends beyond file";
                return false;
            }
            
            const uint8_t* execInfo = data + value;
            
            // Execution info structure:
            // 0x00: media_id (4)
            // 0x04: version (4)
            // 0x08: base_version (4)
            // 0x0C: title_id (4)
            // 0x10: platform (1)
            // 0x11: executable_table (1)
            // 0x12: disc_number (1)
            // 0x13: disc_count (1)
            // 0x14: savegame_id (4)
            
            result.mediaId = be32(execInfo + 0x00);
            result.version = be32(execInfo + 0x04);
            result.titleId = be32(execInfo + 0x0C);
            result.discNumber = execInfo[0x12];
            result.discCount = execInfo[0x13];
            
            return true;
        }
    }
    
    result.errorMessage = "Execution info header not found";
    return false;
}

std::string XexValidator::getRegionName(uint32_t region) {
    if (region == static_cast<uint32_t>(XexRegion::All)) {
        return "Region Free";
    }
    
    std::string names;
    
    if (region & static_cast<uint32_t>(XexRegion::NTSCU)) {
        if (!names.empty()) names += ", ";
        names += "NTSC-U (USA)";
    }
    if (region & static_cast<uint32_t>(XexRegion::NTSCJ)) {
        if (!names.empty()) names += ", ";
        names += "NTSC-J (Japan)";
    }
    if (region & static_cast<uint32_t>(XexRegion::PAL)) {
        if (!names.empty()) names += ", ";
        names += "PAL (Europe)";
    }
    if (region & static_cast<uint32_t>(XexRegion::Other)) {
        if (!names.empty()) names += ", ";
        names += "Other";
    }
    
    return names.empty() ? "Unknown" : names;
}

bool XexValidator::isRegionCompatible(uint32_t xexRegion, uint32_t requiredRegion) {
    // Region-free games are always compatible
    if (xexRegion == static_cast<uint32_t>(XexRegion::All)) {
        return true;
    }
    
    // Check if required region bits are set
    return (xexRegion & requiredRegion) != 0;
}

} // namespace install
} // namespace liberty
