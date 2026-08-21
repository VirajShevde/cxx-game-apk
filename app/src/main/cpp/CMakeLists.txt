cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# Point to the directory containing the SDL2 folder so #include <SDL2/SDL.h> works
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)

find_library(
    log-lib
    log
)

target_link_libraries(
    cxxgame
    ${log-lib}
)
