cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# --------------------------------------------------
# C++ configuration
# --------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------
# SDL2 paths supplied by Gradle
# --------------------------------------------------

set(SDL2_INCLUDE_DIR "${SDL2_INCLUDE_DIR}" CACHE PATH "SDL2 include directory")
set(SDL2_LIB_DIR "${SDL2_LIB_DIR}" CACHE PATH "SDL2 library directory")

include_directories(
    "${SDL2_INCLUDE_DIR}"
    "${SDL2_INCLUDE_DIR}/SDL2"
)

# --------------------------------------------------
# Main game library
# --------------------------------------------------

add_library(
    cxxgame
    SHARED
    game57_final_ai_map_generator_railway_assets_patched-1_build_fixed_v7.cxx
)

# --------------------------------------------------
# SDL2
# --------------------------------------------------

add_library(SDL2 SHARED IMPORTED)

set_target_properties(
    SDL2
    PROPERTIES
    IMPORTED_LOCATION
    "${SDL2_LIB_DIR}/libSDL2.so"
    INTERFACE_INCLUDE_DIRECTORIES
    "${SDL2_INCLUDE_DIR}"
)

# --------------------------------------------------
# SDL2 libraries
# --------------------------------------------------

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

# --------------------------------------------------
# Link
# --------------------------------------------------

target_link_libraries(
    cxxgame
    SDL2
    ${log-lib}
    ${android-lib}
    ${dl-lib}
    ${GLES2-lib}
)
