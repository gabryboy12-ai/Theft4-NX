/**
 ******************************************************************************
 * ReXGlue Runtime                                                            *
 ******************************************************************************
 * Copyright 2026. Released under the BSD license.                            *
 ******************************************************************************
 */

#pragma once

#include <spdlog/common.h>

namespace rex {

// The level of every category logger, including the ones created later
// (SetAllLevels only reaches the loggers that exist already; categories such
// as the kernel's are created on first use with the configured default).
void SetDefaultLevel(spdlog::level::level_enum level);

}  // namespace rex
