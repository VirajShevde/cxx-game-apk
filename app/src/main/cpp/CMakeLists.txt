cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Look ONLY in the unified headers directory
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_headers
)

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)

find_library(
    log-lib
    log
)

# Find SDL2 libraries (prebuilt in NDK)
find_library(SDL2_LIB SDL2)
find_library(SDL2_image_LIB SDL2_image)

target_link_libraries(
    cxxgame
    ${log-lib}
    ${SDL2_LIB}
    ${SDL2_image_LIB}
)
