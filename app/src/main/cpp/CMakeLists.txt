cmake_minimum_required(VERSION 3.22.1)

project(MyGame LANGUAGES C CXX)

--------------------------------------------------

C++ SETTINGS

--------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

--------------------------------------------------

SDL2 SETTINGS

--------------------------------------------------

set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

--------------------------------------------------

BUILD SDL2 FROM SOURCE

Expected structure:

cpp/

├── CMakeLists.txt

├── main.cpp

└── SDL/

├── CMakeLists.txt

├── include/

└── src/

--------------------------------------------------

add_subdirectory(
${CMAKE_CURRENT_SOURCE_DIR}/SDL
${CMAKE_CURRENT_BINARY_DIR}/SDL-build
)

--------------------------------------------------

YOUR GAME LIBRARY

--------------------------------------------------

add_library(
native-lib
SHARED

main.cpp

)

--------------------------------------------------

INCLUDE DIRECTORIES

--------------------------------------------------

target_include_directories(
native-lib
PRIVATE

${CMAKE_CURRENT_SOURCE_DIR}/SDL/include

)

--------------------------------------------------

ANDROID LIBRARIES

--------------------------------------------------

find_library(
log-lib
log
)

find_library(
android-lib
android
)

--------------------------------------------------

LINK LIBRARIES

--------------------------------------------------

target_link_libraries(
native-lib
PRIVATE

SDL2

${android-lib}
${log-lib}

)
