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

# Find SDL2 libraries
# Search in the directory provided by Gradle build system
# Don't require SDL2 lib to be found - we'll handle it gracefully
find_library(
    SDL2_LIB
    SDL2
    HINTS ${SDL2_LIB_DIR}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)

find_library(
    SDL2_image_LIB
    SDL2_image
    HINTS ${SDL2_LIB_DIR}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)

# Check if libraries were found
if(SDL2_LIB)
    message(STATUS "Found SDL2 library: ${SDL2_LIB}")
else()
    message(WARNING "SDL2 library not found at ${SDL2_LIB_DIR}")
    message(STATUS "This may be expected if SDL2 is provided as a system library")
    # Create a dummy library target to allow compilation to proceed
    set(SDL2_LIB "")
endif()

if(SDL2_image_LIB)
    message(STATUS "Found SDL2_image library: ${SDL2_image_LIB}")
else()
    message(WARNING "SDL2_image library not found at ${SDL2_LIB_DIR}")
    set(SDL2_image_LIB "")
endif()

# Link libraries - only link if they were found
target_link_libraries(
    cxxgame
    ${log-lib}
)

if(SDL2_LIB)
    target_link_libraries(cxxgame ${SDL2_LIB})
endif()

if(SDL2_image_LIB)
    target_link_libraries(cxxgame ${SDL2_image_LIB})
endif()

message(STATUS "Build configuration:")
message(STATUS "  Target: cxxgame")
message(STATUS "  SDL2 Include: ${SDL2_INCLUDE_DIR}")
message(STATUS "  SDL2 Lib Dir: ${SDL2_LIB_DIR}")
message(STATUS "  SDL2 Library: ${SDL2_LIB}")
message(STATUS "  SDL2_image Library: ${SDL2_image_LIB}")
