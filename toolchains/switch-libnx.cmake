# Nintendo Switch / libnx (devkitPro) Toolchain
# Requirements:
#   - devkitPro with devkitA64 and libnx installed
#     Install: https://devkitpro.org/wiki/Getting_Started
#     Then:    dkp-pacman -S switch-dev
#   - Environment: DEVKITPRO and DEVKITA64 set by devkitPro's env.sh
#
# Setup:
#   source $DEVKITPRO/devkita64/environment-setup-aarch64-none-elf
#   cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/switch-libnx.cmake \
#         -DLIBERTY_RECOMP_TARGET_PLATFORM=switch \
#         -DLIBERTY_RECOMP_EMBEDDED_GAME_PATH=tools/local_game_payload \
#         ..
#
# Verify installed packages:
#   dkp-pacman -Q switch-dev

# Tell rexglue SDK we're targeting Switch console
# (lowercase — graine's console branches check for "switch")
set(LIBERTY_RECOMP_TARGET_PLATFORM "switch" CACHE STRING "" FORCE)

# Resolve devkitPro paths
if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        set(DEVKITPRO "/opt/devkitpro")  # common default
    endif()
endif()

if(NOT DEFINED DEVKITA64)
    if(DEFINED ENV{DEVKITA64})
        set(DEVKITA64 "$ENV{DEVKITA64}")
    else()
        set(DEVKITA64 "${DEVKITPRO}/devkitA64")
    endif()
endif()

# Resolve repo-local libnx
get_filename_component(_switch_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LIBNX_ROOT "${_switch_repo_root}/tools/libnx")

if(EXISTS "${DEVKITA64}")
    # ── Mode A: devkitPro's official Switch toolchain ────────────────────────
    # $DEVKITPRO/cmake/Switch.cmake owns the cross-toolchain setup:
    #   - devkitA64.cmake: aarch64-none-elf gcc/g++/gcc-ar/gcc-ranlib/ld/strip
    #   - CMAKE_SYSTEM_NAME NintendoSwitch -> Platform/NintendoSwitch.cmake:
    #       -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft
    #       -ftls-model=local-exec -ffunction-sections -fdata-sections
    #       -D__SWITCH__, -fPIE -specs=libnx/switch.specs, -lnx -lm,
    #       libnx/include, CMAKE_POSITION_INDEPENDENT_CODE ON
    #   - search prefixes: portlibs/switch and libnx (mesa-switch libvulkan.a
    #     lives in portlibs/switch/lib), host PATH excluded
    # None of that is repeated here.
    message(STATUS "Using devkitA64: ${DEVKITA64}")
    message(STATUS "Using devkitPro: ${DEVKITPRO}")

    # devkitPro's files re-derive DEVKITPRO from the environment.
    if(NOT DEFINED ENV{DEVKITPRO})
        set(ENV{DEVKITPRO} "${DEVKITPRO}")
    endif()
    include("${DEVKITPRO}/cmake/Switch.cmake")

    # Platform/NintendoSwitch.cmake assigns CMAKE_<LANG>_FLAGS_INIT and
    # CMAKE_EXE_LINKER_FLAGS_INIT after this file runs, so Theft4's own flags
    # are appended from a rules override that CMake evaluates after it.
    set(CMAKE_USER_MAKE_RULES_OVERRIDE
        "${CMAKE_CURRENT_LIST_DIR}/switch-libnx-rules.cmake")
else()
    # ── Mode B: Clang cross-compile (no devkitPro) — REMOVED ─────────────────
    # Building Switch homebrew without devkitPro is not viable:
    #
    #   1. tools/libnx/nx/Makefile hard-requires $DEVKITPRO and includes
    #      $(DEVKITPRO)/devkitA64/base_rules — libnx cannot be built against
    #      a bare Homebrew LLVM toolchain (no newlib sysroot, no base_rules).
    #   2. No prebuilt libnx.a ships in this repo; tools/libnx/ is source-only,
    #      so there is nothing to link against without a devkitPro build first.
    #   3. .nro packaging requires elf2nro and nacptool from devkitPro — even
    #      a successful ELF link could not be turned into a runnable homebrew.
    #   4. The previous Mode B injected macOS libc++ headers into an
    #      aarch64-none-elf newlib build, which prior review flagged as
    #      fundamentally broken (ABI/layout mismatch, host-only intrinsics).
    #
    # For a Switch build, install devkitPro:
    #   https://devkitpro.org/wiki/Getting_Started
    # Then:
    #   dkp-pacman -S switch-dev
    #   source $DEVKITPRO/devkita64/environment-setup-aarch64-none-elf
    #   cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/switch-libnx.cmake ...
    message(FATAL_ERROR
        "Switch build requires devkitPro (devkitA64 + libnx).\n"
        "devkitA64 not found at: ${DEVKITA64}\n"
        "\n"
        "Install devkitPro: https://devkitpro.org/wiki/Getting_Started\n"
        "  then:  dkp-pacman -S switch-dev\n"
        "  then:  source \$DEVKITPRO/devkita64/environment-setup-aarch64-none-elf\n"
        "\n"
        "If devkitPro is installed in a non-standard location, set the\n"
        "DEVKITPRO environment variable (or pass -DDEVKITPRO=<path>) before\n"
        "re-running CMake.\n"
        "\n"
        "The previous 'Mode B' Clang cross-compile path has been removed:\n"
        "it could not build libnx, could not package a .nro, and leaked\n"
        "host libc++ headers into the aarch64-none-elf newlib build."
    )
endif()

# Compile flags for Switch homebrew: -march/-mtune/-mtp/-D__SWITCH__/-fPIE come
# from Platform/NintendoSwitch.cmake; -D_GNU_SOURCE and -DSPDLOG_NO_TZ_OFFSET
# are appended in switch-libnx-rules.cmake.

# newlib requires GNU extensions for POSIX visibility (__POSIX_VISIBLE, __MISC_VISIBLE).
# Without this, -std=c++23 hides fileno, isatty, fwrite_unlocked, etc.
set(CMAKE_CXX_EXTENSIONS ON)

# Host libraries are kept out by devkitPro's search setup (portlibs/switch and
# libnx prefixes, CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF).

# Switch uses Vulkan via deko3d or SDL2's libnx backend
set(LIBERTY_RECOMP_VULKAN ON CACHE BOOL "Use Vulkan/deko3d renderer" FORCE)
set(LIBERTY_RECOMP_D3D12 OFF CACHE BOOL "" FORCE)
set(LIBERTY_RECOMP_METAL OFF CACHE BOOL "" FORCE)

# No installer wizard on Switch
set(LIBERTY_RECOMP_EMBEDDED_ASSETS ON CACHE BOOL "Package assets at build time (no installer UI)" FORCE)

# NRO (homebrew) output settings
set(LIBERTY_SWITCH_APP_TITLE "Liberty Recomp" CACHE STRING "Switch homebrew title")
set(LIBERTY_SWITCH_APP_AUTHOR "LibertyRecomp Team" CACHE STRING "Switch homebrew author")
set(LIBERTY_SWITCH_APP_VERSION "1.0.0" CACHE STRING "Switch homebrew version")
