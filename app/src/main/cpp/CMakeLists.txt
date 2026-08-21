cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 include directories - passed from Gradle
# Fall back to project local headers if paths not provided
if(NOT SDL2_INCLUDE_DIR)
    set(SDL2_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_headers)
endif()

include_directories(${SDL2_INCLUDE_DIR})

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)

find_library(
    log-lib
    log
)

# Find SDL2 libraries
# Search in the directory provided by Gradle build system
find_library(
    SDL2_LIB
    SDL2
    HINTS ${SDL2_LIB_DIR}
    REQUIRED
)

find_library(
    SDL2_image_LIB
    SDL2_image
    HINTS ${SDL2_LIB_DIR}
    REQUIRED
)

target_link_libraries(
    cxxgame
    ${log-lib}
    ${SDL2_LIB}
    ${SDL2_image_LIB}
)
