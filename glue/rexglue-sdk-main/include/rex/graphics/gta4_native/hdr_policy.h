#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace rex::graphics::gta4_native {

enum class HdrMode : uint8_t {
  kOff,
  kScRgb,
  kAutoHdr,
};

enum class HdrCompatibility : uint8_t {
  kCanonical,
  kLegacyVulkanHdr,
  kFallbackOff,
};

struct ResolvedHdrConfiguration {
  HdrMode mode = HdrMode::kOff;
  HdrCompatibility compatibility = HdrCompatibility::kCanonical;
};

constexpr std::optional<HdrMode> ParseHdrMode(std::string_view value) {
  if (value == "off") {
    return HdrMode::kOff;
  }
  if (value == "scrgb") {
    return HdrMode::kScRgb;
  }
  if (value == "auto_hdr") {
    return HdrMode::kAutoHdr;
  }
  return std::nullopt;
}

constexpr std::string_view HdrModeName(HdrMode mode) {
  switch (mode) {
    case HdrMode::kOff:
      return "off";
    case HdrMode::kScRgb:
      return "scrgb";
    case HdrMode::kAutoHdr:
      return "auto_hdr";
  }
  return "off";
}

constexpr bool HdrModeNeedsExtendedOutput(HdrMode mode) {
  return mode != HdrMode::kOff;
}

constexpr ResolvedHdrConfiguration ResolveHdrConfiguration(
    std::string_view configured_mode, bool legacy_vulkan_hdr,
    bool unified_mode_selected) {
  if (const auto canonical = ParseHdrMode(configured_mode)) {
    if (unified_mode_selected || *canonical != HdrMode::kOff || !legacy_vulkan_hdr) {
      return {*canonical, HdrCompatibility::kCanonical};
    }
  }
  if (legacy_vulkan_hdr) {
    return {HdrMode::kScRgb, HdrCompatibility::kLegacyVulkanHdr};
  }
  return {HdrMode::kOff, HdrCompatibility::kFallbackOff};
}

void InitializeHdrController();
HdrMode GetConfiguredHdrMode();
std::string_view GetConfiguredHdrModeName();
bool SetConfiguredHdrMode(std::string_view value);

}  // namespace rex::graphics::gta4_native
