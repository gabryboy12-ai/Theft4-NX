cmake_minimum_required(VERSION 3.16)
project(liberty_debugger)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(DEBUGGER_SOURCES
    debugger.cpp
    debug_state.cpp
    command_parser.cpp
)

set(DEBUGGER_HEADERS
    debugger.h
    debug_state.h
    command_parser.h
)

# Build as static library for linking into LibertyRecomp
add_library(liberty_debugger STATIC ${DEBUGGER_SOURCES} ${DEBUGGER_HEADERS})

target_include_directories(liberty_debugger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(liberty_debugger PRIVATE __APPLE__)
elseif(WIN32)
    target_compile_definitions(liberty_debugger PRIVATE _WIN32)
elseif(UNIX)
    target_compile_definitions(liberty_debugger PRIVATE __linux__)
endif()

# Enable warnings
if(MSVC)
    target_compile_options(liberty_debugger PRIVATE /W4)
else()
    target_compile_options(liberty_debugger PRIVATE -Wall -Wextra -Wpedantic)
endif()
