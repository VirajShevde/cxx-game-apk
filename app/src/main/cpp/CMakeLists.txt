cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 include directories - passed from Gradle
# Fall back to project local headers if paths not provided
if(NOT SDL2_INCLUDE_DIR)
    set(SDL2_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_headers)
endif()

message(STATUS "SDL2_INCLUDE_DIR: ${SDL2_INCLUDE_DIR}")
message(STATUS "SDL2_LIB_DIR: ${SDL2_LIB_DIR}")

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

# Link libraries - link SDL2 and SDL2_image by name
# They are provided by the Android NDK or as system libraries
target_link_libraries(
    cxxgame
    ${log-lib}
    SDL2
    SDL2_image
)

message(STATUS "Build configuration:")
message(STATUS "  Target: cxxgame")
message(STATUS "  SDL2 Include: ${SDL2_INCLUDE_DIR}")
message(STATUS "  SDL2 Lib Dir: ${SDL2_LIB_DIR}")
message(STATUS "  Linking SDL2 and SDL2_image by name")
