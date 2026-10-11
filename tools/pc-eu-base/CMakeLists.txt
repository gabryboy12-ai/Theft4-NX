# pc-eu-base: the code generated for Switch (gta4-recomp/generated/eu-base,
# config-eu-base: European base default.xex, no hooks, no TU) built for
# Windows with ReXGlue's graphics, to see how far the translation gets on a
# PC (docs/switch-port/06-game-boot-plan.md section 14).
#
# Theft4's desktop settings for ReXGlue (glue/CMakeLists.txt: Vulkan on, D3D12
# off, FidelityFX off) and the SDK's Windows compiler settings (clang from
# Visual Studio, -march=x86-64-v3, C++23). Theft4's own desktop presets
# (x64-Clang-*) build LibertyRecomp, the XenonRecomp/plume path, and
# gta4-recomp builds an application only on macOS: hence this project.
#
# From a Visual Studio x64 environment (vcvars64 + VC/Tools/Llvm/x64/bin first
# on PATH, Visual Studio's CMake and Ninja), see build.bat:
#   cmake -S tools/pc-eu-base -B out/build/pc-eu-base -G Ninja
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
#   ninja -C out/build/pc-eu-base -j3      (never more on the generated files)

cmake_minimum_required(VERSION 3.25)
project(theft4_pc_eu_base LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "pc-eu-base is the Windows build of the Switch game's generated code")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3")

get_filename_component(THEFT4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(THEFT4_SDK_DIR "${THEFT4_ROOT}/glue/rexglue-sdk-main")
set(THEFT4_GENERATED_DIR "${THEFT4_SDK_DIR}/gta4-recomp/generated/eu-base")

if(NOT EXISTS "${THEFT4_GENERATED_DIR}/sources.cmake")
  message(FATAL_ERROR "Generate the code first: rexglue codegen "
                      "config-eu-base/gta4_eu_base_manifest.toml (06 section 6)")
endif()

# Theft4 desktop (glue/CMakeLists.txt).
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
# gta4-native needs the private shader cache; xenos is the plugin used here.
set(REXGLUE_BUILD_GTA4_NATIVE OFF CACHE BOOL "" FORCE)

# The export trace switch-game has on Switch (rex/hook.h), for comparison.
add_compile_definitions(REX_TRACE_EXPORTS=1)

add_subdirectory("${THEFT4_SDK_DIR}" rexglue-sdk)

include("${THEFT4_GENERATED_DIR}/sources.cmake")

add_executable(theft4-eu-base WIN32
  main.cpp
  ${GENERATED_SOURCES}
)
target_include_directories(theft4-eu-base PRIVATE "${THEFT4_GENERATED_DIR}")

# Direct3D census (tools/d3d_census.py; 06 section 11, phase (b)): every call
# into the XDK's Direct3D library from another translation unit counted
# through lld-link -wrap:, dumped every 10 s to d3d-census.log. Off by default.
option(THEFT4_D3D_CENSUS "Count the calls of every Direct3D library function" OFF)
if(THEFT4_D3D_CENSUS)
  set(THEFT4_D3D_CENSUS_DIR "${THEFT4_ROOT}/out/d3d-census")
  if(NOT EXISTS "${THEFT4_D3D_CENSUS_DIR}/d3d_census_hooks.cpp")
    message(FATAL_ERROR "Run python tools/d3d_census.py first")
  endif()
  target_sources(theft4-eu-base PRIVATE "${THEFT4_D3D_CENSUS_DIR}/d3d_census_hooks.cpp")
  # clang++ expands the file itself: its lines are "-Xlinker -wrap:<symbol>".
  target_link_options(theft4-eu-base PRIVATE "@${THEFT4_D3D_CENSUS_DIR}/d3d_census_wrap.rsp")
endif()
# rexui brings imgui and SDL3, which rex_app.cpp and the SDL main need.
target_link_libraries(theft4-eu-base PRIVATE rex::runtime rexui)
rexglue_configure_target(theft4-eu-base GPU_PLUGINS xenos)
