
cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# ============================================================
# C++ configuration
# ============================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# SDL2 source
# ============================================================

set(SDL2_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/SDL2"
)

if(NOT EXISTS "${SDL2_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL2 source was not found at ${SDL2_DIR}"
    )
endif()

# Build SDL2 as a shared library.
set(SDL_SHARED ON CACHE BOOL "Build SDL2 as shared library" FORCE)
set(SDL_STATIC OFF CACHE BOOL "Build SDL2 as static library" FORCE)

# Disable SDL2 optional tests/examples.
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${SDL2_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/SDL2"
)

# ============================================================
# SDL2_image
# ============================================================

set(SDL2_IMAGE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/SDL2_image"
)

if(NOT EXISTS "${SDL2_IMAGE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL2_image source was not found at ${SDL2_IMAGE_DIR}"
    )
endif()

# Do not build SDL2_image tests.
set(SDL2IMAGE_TESTS OFF CACHE BOOL "" FORCE)

# Use the SDL2 target built above.
set(SDL2IMAGE_VENDORED ON CACHE BOOL "" FORCE)

add_subdirectory(
    "${SDL2_IMAGE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/SDL2_image"
)

# ============================================================
# Main game library
# ============================================================

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)

# ============================================================
# Include directories
# ============================================================

target_include_directories(
    cxxgame
    PRIVATE
    "${SDL2_DIR}/include"
    "${SDL2_IMAGE_DIR}"
)

# ============================================================
# Android system libraries
# ============================================================

find_library(
    log-lib
    log
)

find_library(
    android-lib
    android
)

find_library(
    dl-lib
    dl
)

find_library(
    GLES2-lib
    GLESv2
)

# ============================================================
# Link
# ============================================================

target_link_libraries(
    cxxgame
    SDL2
    SDL2_image
    ${log-lib}
    ${android-lib}
    ${dl-lib}
    ${GLES2-lib}
)
