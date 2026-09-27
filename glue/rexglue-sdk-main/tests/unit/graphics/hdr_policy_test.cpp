#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/gta4_native/hdr_policy.h>

namespace rex::graphics::gta4_native {

TEST_CASE("HDR policy parses only canonical public values") {
  CHECK(ParseHdrMode("off") == HdrMode::kOff);
  CHECK(ParseHdrMode("scrgb") == HdrMode::kScRgb);
  CHECK(ParseHdrMode("auto_hdr") == HdrMode::kAutoHdr);
  CHECK_FALSE(ParseHdrMode("true"));
  CHECK_FALSE(ParseHdrMode("hdr"));
}

TEST_CASE("HDR policy migrates the legacy Vulkan toggle without enabling Auto HDR") {
  const auto migrated = ResolveHdrConfiguration("off", true, false);
  CHECK(migrated.mode == HdrMode::kScRgb);
  CHECK(migrated.compatibility == HdrCompatibility::kLegacyVulkanHdr);
  CHECK(HdrModeNeedsExtendedOutput(migrated.mode));
}

TEST_CASE("HDR policy preserves explicit canonical choices") {
  CHECK(ResolveHdrConfiguration("auto_hdr", false, false).mode == HdrMode::kAutoHdr);
  CHECK(ResolveHdrConfiguration("scrgb", false, true).mode == HdrMode::kScRgb);
  CHECK(ResolveHdrConfiguration("off", true, true).mode == HdrMode::kOff);
  CHECK_FALSE(HdrModeNeedsExtendedOutput(HdrMode::kOff));
}

TEST_CASE("HDR policy falls back safely for malformed configuration") {
  const auto fallback = ResolveHdrConfiguration("invalid", false, false);
  CHECK(fallback.mode == HdrMode::kOff);
  CHECK(fallback.compatibility == HdrCompatibility::kFallbackOff);
}

}  // namespace rex::graphics::gta4_native
