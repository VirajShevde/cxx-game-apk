cmake_minimum_required(VERSION 3.22.1)

project(cxxgame)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================================
# FetchContent
# =========================================================

include(FetchContent)

# =========================================================
# SDL2 2.30.12
# =========================================================

FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
)

FetchContent_MakeAvailable(SDL2)

# =========================================================
# SDL2_image 2.8.1
# =========================================================

FetchContent_Declare(
    SDL2_image
    URL https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.1/SDL2_image-2.8.1.tar.gz
)

FetchContent_MakeAvailable(SDL2_image)

# =========================================================
# Game library
# =========================================================

add_library(
    main
    SHARED
    game57-4.cxx
)

# =========================================================
# Include directories
# =========================================================

target_include_directories(
    main
    PRIVATE
    ${sdl2_SOURCE_DIR}/include
    ${sdl2_image_SOURCE_DIR}/include
)

# =========================================================
# Link SDL
# =========================================================

target_link_libraries(
    main
    SDL2
    SDL2_image
    log
    android
)
