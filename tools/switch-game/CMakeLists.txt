# switch-game: experimental Nintendo Switch game runtime with native audio and Vulkan
# (docs/switch-port/06-game-boot-plan.md section 4). Switch-only project like
# tools/switch-smoke: the SDK as a subdirectory with the headless Switch
# configuration, the generated code of the European base executable (option C)
# and main.cpp. No LTO.
#
#   cmake -S tools/switch-game -B out/build/switch-game -G Ninja \
#         -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/switch-libnx.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   ninja -C out/build/switch-game -j3   # -> out/build/switch-game/switch-game.nro
#
# At most 3 jobs: a generated file takes 1-2 GB of RAM in GCC.

cmake_minimum_required(VERSION 3.25)
project(switch-game VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH OR NOT LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "switch")
    message(FATAL_ERROR "switch-game is Switch-only: configure with toolchains/switch-libnx.cmake")
endif()

get_filename_component(THEFT4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(THEFT4_GENERATED_DIR "${THEFT4_ROOT}/glue/rexglue-sdk-main/gta4-recomp/generated/eu-base"
    CACHE PATH "Output of rexglue codegen for the executable this NRO runs")
if(NOT EXISTS "${THEFT4_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR "No generated code in ${THEFT4_GENERATED_DIR}: run rexglue codegen "
                        "config-eu-base/gta4_eu_base_manifest.toml from gta4-recomp first")
endif()
include("${THEFT4_GENERATED_DIR}/sources.cmake")  # GENERATED_SOURCES
# A static recompilation must load precisely the executable it was generated from.
set(THEFT4_EXPECTED_XEX_SHA256 "74d480d29b513793938b602e0db0b0885a19b102194da71a70db443648b18c9a"
    CACHE STRING "SHA256 of default.xex used to generate THEFT4_GENERATED_DIR")
string(LENGTH "${THEFT4_EXPECTED_XEX_SHA256}" _xex_hash_length)
if(NOT _xex_hash_length EQUAL 64 OR NOT THEFT4_EXPECTED_XEX_SHA256 MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "THEFT4_EXPECTED_XEX_SHA256 must be a 64-digit SHA256")
endif()
string(TOLOWER "${THEFT4_EXPECTED_XEX_SHA256}" THEFT4_EXPECTED_XEX_SHA256)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
# Phase (a) of the renderer (06-game-boot-plan.md section 24): ReXGlue's Vulkan
# backend (rexgpu-xenos-nx) and NVK linked in; the cvar nx_gpu picks it at run
# time. Native Vulkan is the default; configure with
# -DTHEFT4_SWITCH_GAME_VULKAN=OFF only for a headless diagnostic build.
option(THEFT4_SWITCH_GAME_VULKAN "Link ReXGlue's Vulkan backend and NVK (cvar nx_gpu)" ON)
set(THEFT4_SWITCH_VULKAN_LIBRARIES "vulkan;expat;zstd;z;nx" CACHE STRING
    "Static Vulkan driver and dependencies; use vulkan;nx for a bundled NVK archive")
set(REXGLUE_SWITCH_VULKAN ${THEFT4_SWITCH_GAME_VULKAN} CACHE BOOL "" FORCE)
add_subdirectory("${THEFT4_ROOT}/glue/rexglue-sdk-main" rexglue-sdk)
option(THEFT4_SWITCH_DIRECT_RESOLVE "Use pinned XeniOS host-image resolve shaders" ON)
if(THEFT4_SWITCH_GAME_VULKAN AND THEFT4_SWITCH_DIRECT_RESOLVE)
    set(_direct_resolve_include "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/xenios-direct-resolve/include")
    if(NOT EXISTS "${_direct_resolve_include}/xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_32bpp_1xmsaa_cs.h")
        message(FATAL_ERROR "Missing pinned direct-resolve shaders; see thirdparty/xenios-direct-resolve/README.md")
    endif()
    # Scope this to the resolve source, preserving the other backend objects.
    set_source_files_properties(
        "${THEFT4_ROOT}/glue/rexglue-sdk-main/src/graphics/vulkan/render_target_cache.cpp"
        TARGET_DIRECTORY rexgpu-xenos-nx PROPERTIES
        COMPILE_DEFINITIONS THEFT4_XENIOS_DIRECT_RESOLVE_SHADERS=1
        INCLUDE_DIRECTORIES "${_direct_resolve_include}")
endif()

# Stubs for the imports the SDK does not define (missing_imports.py), from the
# archives actually linked; regenerated when they or the generated header change.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# missing_imports.py calls the devkitPro nm (an MSYS path): run it with the
# MSYS python even when the Vulkan build points Python3_EXECUTABLE at
# native_python.sh for SPIRV-Tools.
set(THEFT4_HOST_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python for missing_imports.py")
set(MISSING_IMPORTS_CPP "${CMAKE_CURRENT_BINARY_DIR}/missing_imports.cpp")
set(MISSING_IMPORTS_TXT "${CMAKE_CURRENT_BINARY_DIR}/missing_imports.txt")
add_custom_command(
    OUTPUT "${MISSING_IMPORTS_CPP}" "${MISSING_IMPORTS_TXT}"
    COMMAND "${THEFT4_HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/missing_imports.py"
            --header "${THEFT4_GENERATED_DIR}/gta4_init.h"
            --nm "${CMAKE_NM}"
            --out "${MISSING_IMPORTS_CPP}"
            --list "${MISSING_IMPORTS_TXT}"
            "$<TARGET_FILE:rexruntime>" "$<TARGET_FILE:rexcore>"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/missing_imports.py"
            "${THEFT4_GENERATED_DIR}/gta4_init.h" rexruntime rexcore
    COMMENT "Stubs for imports the SDK does not define"
    VERBATIM
)

add_executable(switch-game main.cpp nx_input.cpp nx_clocks.cpp nx_profiler.cpp nx_threads.cpp
    "${THEFT4_ROOT}/tools/switch-common/nx_exit.cpp"
    "${MISSING_IMPORTS_CPP}" ${GENERATED_SOURCES})
target_compile_features(switch-game PRIVATE cxx_std_23)
target_compile_definitions(switch-game PRIVATE
    THEFT4_EXPECTED_XEX_SHA256="${THEFT4_EXPECTED_XEX_SHA256}")
target_include_directories(switch-game PRIVATE
    "${THEFT4_GENERATED_DIR}"
    "${THEFT4_ROOT}/tools/switch-common"
    "${THEFT4_ROOT}/glue/rexglue-sdk-main/include"
)
# The SDK's own global options (glue/rexglue-sdk-main/CMakeLists.txt), which do
# not reach this directory: the generated code needs the same aliasing and
# floating-point model as the runtime it calls into.
target_compile_options(switch-game PRIVATE
    -fno-strict-aliasing
    -ffp-contract=off -frounding-math -ftrapping-math
    -fno-char8_t
    -mcmodel=small
    $<$<CONFIG:RELEASE>:-O3>
)
# Line tables only for the ~2 MB generated files: enough for addr2line on a
# crash report, a fraction of the memory and time of full -g.
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS "-g1")
# The registration routine contains ~37,000 consecutive virtual calls and only
# runs at startup. GCC's -O3 processing of this single basic block takes minutes
# and substantial memory; gameplay functions above retain their -O3 setting.
set_source_files_properties("${THEFT4_GENERATED_DIR}/gta4_register.cpp"
    PROPERTIES COMPILE_OPTIONS "-g1;-O1")

target_link_libraries(switch-game PRIVATE
    -Wl,--start-group
    rexruntime
    rexcore
    mspack
    aes128
    tiny-aes
    o1heap
    xxHash::xxhash
    libavcodec
    libavutil
    fmt::fmt
    spdlog::spdlog
    -Wl,--end-group
)
target_link_options(switch-game PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/switch-game.map")
if(THEFT4_SWITCH_GAME_VULKAN)
    # The whole backend archive (its cvars and the Window::Create of the NWindow
    # register or are found only that way), then NVK as switch-smoke links it:
    # Mesa's static libvulkan.a and its dependencies (vulkan.pc), libelf stubbed.
    # main.cpp only: the generated sources keep their flags and objects (the
    # backend is linked LINK_ONLY below, so its usage requirements stay out).
    set_source_files_properties(main.cpp PROPERTIES
        COMPILE_DEFINITIONS "THEFT4_NX_VULKAN=1;REX_HAS_VULKAN=1;THEFT4_NX_DIRECT_RESOLVE=$<BOOL:${THEFT4_SWITCH_DIRECT_RESOLVE}>"
        INCLUDE_DIRECTORIES
            "$<TARGET_PROPERTY:Vulkan-Headers,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>;${THEFT4_ROOT}/glue/rexglue-sdk-main/thirdparty/renderdoc")
    target_sources(switch-game PRIVATE "${THEFT4_ROOT}/tools/switch-smoke/nx_elf_stubs.c")
    target_link_options(switch-game PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:rexgpu-xenos-nx>,--no-whole-archive")
    set_property(TARGET switch-game APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:rexgpu-xenos-nx>")
    target_link_directories(switch-game PRIVATE "${DEVKITPRO}/portlibs/switch/lib")
    target_link_libraries(switch-game PRIVATE $<LINK_ONLY:rexgpu-xenos-nx>
        -Wl,--start-group ${THEFT4_SWITCH_VULKAN_LIBRARIES} -Wl,--end-group)
    # Run 21: NVK's nvdrv calls go through main.cpp's counters first (fence
    # waits and queries, kickoffs, nvmaps), as sa2-nx's switch_perf does.
    # Run 3: and its GPU mappings, to turn the GPU's L2 on (cvar nx_gpu_l2).
    target_link_options(switch-game PRIVATE
        "-Wl,--wrap=nvFenceWait,--wrap=nvGpuChannelKickoff,--wrap=nvMapCreate,--wrap=nvMapClose,--wrap=nvioctlNvhostAsGpu_MapBufferEx,--wrap=nvGpuChannelGetErrorNotification")
endif()
# Every way out of the process goes through main.cpp's __wrap_* first, which
# logs the code and the caller before the real function runs.
# Run 22: libnx's fatalThrow, diagAbortWithResult and svcBreak too (crash log
# with the result and the caller, then the real function).
target_link_options(switch-game PRIVATE
    "-Wl,--wrap=exit,--wrap=_exit,--wrap=_Exit,--wrap=abort,--wrap=svcExitProcess"
    "-Wl,--wrap=fatalThrow,--wrap=diagAbortWithResult,--wrap=svcBreak")
# Run 23: every thread through nx_threads.cpp (stack range, stack watch, start
# routine of a pthread; libnx's newlib glue creates pthreads with threadCreate).
target_link_options(switch-game PRIVATE
    "-Wl,--wrap=threadCreate,--wrap=threadClose,--wrap=pthread_create")

nx_generate_nacp(switch-game.nacp
    NAME "Theft4 switch-game"
    AUTHOR "Theft4"
    VERSION "${PROJECT_VERSION}"
)
set(THEFT4_SWITCH_NRO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/switch-game.nro" CACHE FILEPATH
    "Output NRO path (may use external build storage)")
nx_create_nro(switch-game
    OUTPUT "${THEFT4_SWITCH_NRO_OUTPUT}"
    NACP switch-game.nacp
)
