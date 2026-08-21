cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# Force C++17 so std::clamp compiles successfully
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_image/include
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

target_link_libraries(
    cxxgame
    ${log-lib}
)
