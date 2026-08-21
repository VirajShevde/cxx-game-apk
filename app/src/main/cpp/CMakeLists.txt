cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# Point directly to the SDL2 include folder inside the downloaded directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include)

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
