#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace rex::graphics::gta4_native {

// Public Advanced Graphics values. Legacy spellings are deliberately excluded
// from this enum and are handled only by ResolveAntiAliasingConfiguration.
enum class AntiAliasingMode : uint8_t {
  kOff,
  kFxaa,
  kSmaa,
  kMsaa2x,
  kMsaa4x,
  kSsaa2x,
  kSsaa4x,
  kSsaa6x,
  kSsaa8x,
  kSsaa10x,
  kSsaa12x,
  kSsaa14x,
  kSsaa16x,
};

enum class AntiAliasingApplyResult : uint8_t {
  kRejected,
  kAppliedLive,
  kRestartRequired,
};

enum class AntiAliasingCompatibility : uint8_t {
  kCanonical,
  kSpatialAlias,
  kLegacySceneMsaa,
  kLegacySpatialToggle,
  kFallbackOff,
};

struct ResolvedAntiAliasingConfiguration {
  AntiAliasingMode mode = AntiAliasingMode::kOff;
  AntiAliasingCompatibility compatibility = AntiAliasingCompatibility::kCanonical;
  bool ignored_legacy_scene_msaa = false;
};

struct AntiAliasingRoute {
  bool presentation_fxaa = false;
  bool presentation_smaa = false;
  uint32_t scene_sample_count = 1u;
  // Total scene pixel-count multiplier. Unlike MSAA, this increases the
  // physical width and height of host-only render targets and is resolved
  // back to the logical output extent before presentation.
  uint32_t supersampling_pixel_factor = 1u;
};

constexpr std::optional<AntiAliasingMode> ParseAntiAliasingMode(std::string_view value) {
  if (value == "off") {
    return AntiAliasingMode::kOff;
  }
  if (value == "fxaa") {
    return AntiAliasingMode::kFxaa;
  }
  if (value == "smaa") {
    return AntiAliasingMode::kSmaa;
  }
  if (value == "msaa2x") {
    return AntiAliasingMode::kMsaa2x;
  }
  if (value == "msaa4x") {
    return AntiAliasingMode::kMsaa4x;
  }
  if (value == "ssaa2x") {
    return AntiAliasingMode::kSsaa2x;
  }
  if (value == "ssaa4x") {
    return AntiAliasingMode::kSsaa4x;
  }
  if (value == "ssaa6x") {
    return AntiAliasingMode::kSsaa6x;
  }
  if (value == "ssaa8x") {
    return AntiAliasingMode::kSsaa8x;
  }
  if (value == "ssaa10x") {
    return AntiAliasingMode::kSsaa10x;
  }
  if (value == "ssaa12x") {
    return AntiAliasingMode::kSsaa12x;
  }
  if (value == "ssaa14x") {
    return AntiAliasingMode::kSsaa14x;
  }
  if (value == "ssaa16x") {
    return AntiAliasingMode::kSsaa16x;
  }
  return std::nullopt;
}

constexpr std::string_view AntiAliasingModeName(AntiAliasingMode mode) {
  switch (mode) {
    case AntiAliasingMode::kOff:
      return "off";
    case AntiAliasingMode::kFxaa:
      return "fxaa";
    case AntiAliasingMode::kSmaa:
      return "smaa";
    case AntiAliasingMode::kMsaa2x:
      return "msaa2x";
    case AntiAliasingMode::kMsaa4x:
      return "msaa4x";
    case AntiAliasingMode::kSsaa2x:
      return "ssaa2x";
    case AntiAliasingMode::kSsaa4x:
      return "ssaa4x";
    case AntiAliasingMode::kSsaa6x:
      return "ssaa6x";
    case AntiAliasingMode::kSsaa8x:
      return "ssaa8x";
    case AntiAliasingMode::kSsaa10x:
      return "ssaa10x";
    case AntiAliasingMode::kSsaa12x:
      return "ssaa12x";
    case AntiAliasingMode::kSsaa14x:
      return "ssaa14x";
    case AntiAliasingMode::kSsaa16x:
      return "ssaa16x";
  }
  return "off";
}

constexpr bool UsesSceneMsaa(AntiAliasingMode mode) {
  return mode == AntiAliasingMode::kMsaa2x || mode == AntiAliasingMode::kMsaa4x;
}

constexpr bool UsesSceneSupersampling(AntiAliasingMode mode) {
  return mode >= AntiAliasingMode::kSsaa2x && mode <= AntiAliasingMode::kSsaa16x;
}

constexpr bool UsesSceneTopology(AntiAliasingMode mode) {
  return UsesSceneMsaa(mode) || UsesSceneSupersampling(mode);
}

constexpr AntiAliasingRoute GetAntiAliasingRoute(AntiAliasingMode mode) {
  switch (mode) {
    case AntiAliasingMode::kFxaa:
      return {.presentation_fxaa = true};
    case AntiAliasingMode::kSmaa:
      return {.presentation_smaa = true};
    case AntiAliasingMode::kMsaa2x:
      return {.scene_sample_count = 2u};
    case AntiAliasingMode::kMsaa4x:
      return {.scene_sample_count = 4u};
    case AntiAliasingMode::kSsaa2x:
      return {.supersampling_pixel_factor = 2u};
    case AntiAliasingMode::kSsaa4x:
      return {.supersampling_pixel_factor = 4u};
    case AntiAliasingMode::kSsaa6x:
      return {.supersampling_pixel_factor = 6u};
    case AntiAliasingMode::kSsaa8x:
      return {.supersampling_pixel_factor = 8u};
    case AntiAliasingMode::kSsaa10x:
      return {.supersampling_pixel_factor = 10u};
    case AntiAliasingMode::kSsaa12x:
      return {.supersampling_pixel_factor = 12u};
    case AntiAliasingMode::kSsaa14x:
      return {.supersampling_pixel_factor = 14u};
    case AntiAliasingMode::kSsaa16x:
      return {.supersampling_pixel_factor = 16u};
    case AntiAliasingMode::kOff:
      return {};
  }
  return {};
}

constexpr bool HasExclusiveAntiAliasingRoute(const AntiAliasingRoute& route) {
  const bool scene_msaa = route.scene_sample_count > 1u;
  const bool scene_ssaa = route.supersampling_pixel_factor > 1u;
  return !(route.presentation_fxaa && route.presentation_smaa) &&
         !(route.presentation_fxaa && scene_msaa) &&
         !(route.presentation_smaa && scene_msaa) &&
         !(route.presentation_fxaa && scene_ssaa) &&
         !(route.presentation_smaa && scene_ssaa) && !(scene_msaa && scene_ssaa);
}

constexpr bool CanApplyAntiAliasingLive(AntiAliasingMode active,
                                        AntiAliasingMode requested) {
  return active == requested || (!UsesSceneTopology(active) && !UsesSceneTopology(requested));
}

// Old configurations stored final-image AA and scene MSAA independently.
// Preserve them deterministically: an explicit post-process mode wins, the
// removed spatial mode becomes FXAA, and only legacy `off` consults the old
// scene-MSAA setting. The old boolean is a last-resort fallback for malformed
// or pre-string configurations.
constexpr ResolvedAntiAliasingConfiguration ResolveAntiAliasingConfiguration(
    std::string_view configured_mode, std::string_view legacy_scene_msaa,
    bool legacy_spatial_enabled, bool unified_mode_selected = false) {
  if (unified_mode_selected) {
    if (const auto canonical = ParseAntiAliasingMode(configured_mode)) {
      return {*canonical, AntiAliasingCompatibility::kCanonical,
              legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
    }
  }
  if (configured_mode == "fxaa") {
    return {AntiAliasingMode::kFxaa, AntiAliasingCompatibility::kCanonical,
            legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
  }
  if (configured_mode == "smaa") {
    return {AntiAliasingMode::kSmaa, AntiAliasingCompatibility::kCanonical,
            legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
  }
  if (configured_mode == "msaa2x") {
    return {AntiAliasingMode::kMsaa2x, AntiAliasingCompatibility::kCanonical,
            legacy_scene_msaa == "4x"};
  }
  if (configured_mode == "msaa4x") {
    return {AntiAliasingMode::kMsaa4x, AntiAliasingCompatibility::kCanonical,
            legacy_scene_msaa == "2x"};
  }
  if (const auto canonical = ParseAntiAliasingMode(configured_mode);
      canonical && UsesSceneSupersampling(*canonical)) {
    return {*canonical, AntiAliasingCompatibility::kCanonical,
            legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
  }
  if (configured_mode == "spatial") {
    return {AntiAliasingMode::kFxaa, AntiAliasingCompatibility::kSpatialAlias,
            legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
  }
  if (configured_mode == "off") {
    if (legacy_scene_msaa == "2x") {
      return {AntiAliasingMode::kMsaa2x, AntiAliasingCompatibility::kLegacySceneMsaa};
    }
    if (legacy_scene_msaa == "4x") {
      return {AntiAliasingMode::kMsaa4x, AntiAliasingCompatibility::kLegacySceneMsaa};
    }
    return {AntiAliasingMode::kOff, AntiAliasingCompatibility::kCanonical};
  }
  if (legacy_spatial_enabled) {
    return {AntiAliasingMode::kFxaa, AntiAliasingCompatibility::kLegacySpatialToggle,
            legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
  }
  return {AntiAliasingMode::kOff, AntiAliasingCompatibility::kFallbackOff,
          legacy_scene_msaa == "2x" || legacy_scene_msaa == "4x"};
}

// Initializes the active renderer route from canonical and legacy config.
// Safe to call repeatedly; initialization is performed once per process.
void InitializeAntiAliasingController();

AntiAliasingMode GetConfiguredAntiAliasingMode();
AntiAliasingMode GetActiveAntiAliasingMode();
std::string_view GetConfiguredAntiAliasingModeName();
std::string_view GetActiveAntiAliasingModeName();

// This is the frontend mutation boundary. Callers must not write the canonical
// cvar directly because MSAA changes require a latched renderer route.
AntiAliasingApplyResult SetConfiguredAntiAliasingMode(std::string_view value);

}  // namespace rex::graphics::gta4_native
