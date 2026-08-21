cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent to download and build SDL2 and SDL2_image
include(FetchContent)

# Fetch and build SDL2
message(STATUS "Fetching SDL2...")
FetchContent_Declare(
    SDL2
    URL "https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz"
    URL_HASH SHA256=0c6ce15aba70e8a9dfc5b14620f57e0f4e1d4dc76669d64faebe15eac28e5590
)
FetchContent_MakeAvailable(SDL2)

# Fetch and build SDL2_image
message(STATUS "Fetching SDL2_image...")
FetchContent_Declare(
    SDL2_image
    URL "https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.1/SDL2_image-2.8.1.tar.gz"
    URL_HASH SHA256=5a88a0e35d54f7d59cb14a09e4f139d4949b4aae2c9d8a6b36db7b5e2e5bfb8f
)
set(SDL2_INCLUDE_DIR ${SDL2_SOURCE_DIR}/include CACHE PATH "SDL2 include directory" FORCE)
FetchContent_MakeAvailable(SDL2_image)

message(STATUS "SDL2 include directory: ${SDL2_INCLUDE_DIR}")
message(STATUS "SDL2_image include directory: ${SDL2_image_SOURCE_DIR}")

# Add the C++ source file
add_library(
    cxxgame
    SHARED
    game57-4.cxx
)

# Find the log library (standard Android library)
find_library(
    log-lib
    log
)

# Link SDL2 and SDL2_image with the built targets
target_link_libraries(
    cxxgame
    ${log-lib}
    SDL2::SDL2
    SDL2_image::SDL2_image
)

message(STATUS "Build configuration:")
message(STATUS "  Target: cxxgame")
message(STATUS "  SDL2 linked: SDL2::SDL2")
message(STATUS "  SDL2_image linked: SDL2_image::SDL2_image")
