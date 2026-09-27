# switch-smoke: first link and on-device smoke test of the ReXGlue SDK on
# Nintendo Switch (libnx). Standalone, Switch-only project; it pulls in the SDK
# as a subdirectory with the same configuration as out/build/switch-sdk and
# links every SDK library the headless runtime uses.
#
#   cmake -S tools/switch-smoke -B out/build/switch-smoke -G Ninja \
#         -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchains/switch-libnx.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   ninja -C out/build/switch-smoke   # -> out/build/switch-smoke/switch-smoke.nro

cmake_minimum_required(VERSION 3.25)
project(switch-smoke VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH OR NOT LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "switch")
    message(FATAL_ERROR "switch-smoke is Switch-only: configure with toolchains/switch-libnx.cmake")
endif()

get_filename_component(THEFT4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${THEFT4_ROOT}/glue/rexglue-sdk-main" rexglue-sdk)

add_executable(switch-smoke main.cpp probes.cpp)
target_compile_features(switch-smoke PRIVATE cxx_std_23)
target_include_directories(switch-smoke PRIVATE "${THEFT4_ROOT}/glue/rexglue-sdk-main/include")

# Every SDK library in the headless Switch runtime (rexruntime already embeds
# the rexfilesystem, rexaudio and kernel objects). The archives are grouped so
# the link does not depend on their order.
target_link_libraries(switch-smoke PRIVATE
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
target_link_options(switch-smoke PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/switch-smoke.map")

nx_generate_nacp(switch-smoke.nacp
    NAME "Theft4 switch-smoke"
    AUTHOR "Theft4"
    VERSION "${PROJECT_VERSION}"
)
nx_create_nro(switch-smoke
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/switch-smoke.nro"
    NACP switch-smoke.nacp
)
