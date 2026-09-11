cmake_minimum_required(VERSION 3.20)
project(DS2SeamlessCoop VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
# The launcher is the original project's; the mod loads as dinput8.dll without it.
option(BUILD_LAUNCHER "Build the launcher application" OFF)
option(BUILD_TESTS "Build test suite" OFF)

# Third-party libraries directory
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# Add MinHook for function hooking
add_subdirectory(${THIRD_PARTY_DIR}/minhook)

# Add ImGui for in-game overlay
add_subdirectory(${THIRD_PARTY_DIR}/imgui)

# Main DLL project
add_subdirectory(src)

# Launcher
if(BUILD_LAUNCHER)
    add_subdirectory(launcher)
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

