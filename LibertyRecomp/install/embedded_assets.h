#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// embedded_assets.h
//
// Build-time asset manifest for platforms that cannot use the installer wizard
// (iOS, Android, PS4, Nintendo Switch).
//
// When LIBERTY_RECOMP_EMBEDDED_ASSETS is defined at compile time, game files
// are bundled directly into the package at the path specified by
// LIBERTY_RECOMP_EMBEDDED_GAME_PATH.  Runtime code should call
// EmbeddedAssets::IsAvailable() first; if it returns true, the installer UI
// should be skipped and EmbeddedAssets::GetGameRoot() used instead.
// ─────────────────────────────────────────────────────────────────────────────

#include <filesystem>
#include <string>
#include <cstdint>

namespace EmbeddedAssets
{
    struct Manifest
    {
        std::string  gameVersion;        // e.g. "1.0.0"
        std::string  titleUpdateVersion; // e.g. "TU8", empty if none
        bool         hasTLAD    = false;
        bool         hasTBOGT   = false;
        uint32_t     buildTimestamp = 0; // Unix timestamp of the build
    };

    /// Returns true when the build was configured with LIBERTY_RECOMP_EMBEDDED_ASSETS.
    bool IsAvailable();

    /// Root directory where embedded game files live at runtime.
    /// Platform-specific:
    ///   iOS    → <Bundle>/Resources/game/
    ///   Android→ <internal_storage>/game/   (extracted from OBB on first boot)
    ///   PS4    → /app0/game/
    ///   Switch → romfs:/game/
    std::filesystem::path GetGameRoot();

    /// Returns the path of the embedded DLC, or empty if not present.
    std::filesystem::path GetDLCRoot();

    /// Reads the manifest JSON written at build time.
    Manifest ReadManifest();

    /// On Android, copies OBB content to internal storage on first boot.
    /// No-op on other platforms.
    bool ExtractObbIfNeeded(const std::filesystem::path& obbPath,
                            const std::filesystem::path& destPath);
}
