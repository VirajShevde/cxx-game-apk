cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

# Add SDL2 include directory (adjust path if your SDL2 headers are located elsewhere)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SDL/include)

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
    # Link your SDL2 libraries here if compiled locally or prebuilt
)
