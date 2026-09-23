# Theft4 additions on top of devkitPro's Switch platform defaults.
# Loaded through CMAKE_USER_MAKE_RULES_OVERRIDE (set in switch-libnx.cmake),
# which CMake evaluates after Platform/NintendoSwitch.cmake has assigned the
# *_FLAGS_INIT values, so appending here is not overwritten.

# Keep devkitPro's own rule overrides (.o extensions, per-config flags).
include("${DEVKITPRO}/cmake/dkp-rule-overrides.cmake")

# -D_GNU_SOURCE enables POSIX visibility in newlib (fileno, isatty, fwrite_unlocked, etc.)
# Without it, -std=c++23 hides these functions.
# -DSPDLOG_NO_TZ_OFFSET: newlib's struct tm lacks tm_gmtoff.
# This file runs once per enabled language; the guards avoid duplicates.
foreach(lang IN ITEMS C CXX)
    if(NOT CMAKE_${lang}_FLAGS_INIT MATCHES "-D_GNU_SOURCE")
        string(APPEND CMAKE_${lang}_FLAGS_INIT " -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET")
    endif()
endforeach()

# -Wl,-Map emits a map file next to the ELF for debugging symbol layout.
if(NOT CMAKE_EXE_LINKER_FLAGS_INIT MATCHES "LibertyRecomp\\.map")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -Wl,-Map,LibertyRecomp.map")
endif()
