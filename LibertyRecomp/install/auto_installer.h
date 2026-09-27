#pragma once

#include <filesystem>
#include <string>

// Result of auto-installation at startup.
struct AutoInstallResult {
    bool tladInstalled  = false;
    bool tbogtInstalled = false;
    bool updateApplied  = false;
    std::string errorMessage;
};

struct AutoInstaller {
    // Silently installs any missing DLC and applies the latest title update.
    // Called once at startup before the wizard check.
    //
    // gamePath: GetGamePath() — the game install root.
    //
    // Source directories are read from Config::DLCSourceDir and
    // Config::UpdateSourceDir. These are written automatically when the
    // installer wizard completes, and can also be set manually in the
    // config file:
    //
    //   [Install]
    //   DLCSourceDir   = "/path/to/folder/containing/dlc/zips"
    //   UpdateSourceDir = "/path/to/folder/containing/update/zips"
    //
    // If both fields are empty this function returns immediately without
    // scanning any directories.
    static AutoInstallResult Run(const std::filesystem::path& gamePath);
};
