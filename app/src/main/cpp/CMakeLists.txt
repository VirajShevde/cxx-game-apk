cmake_minimum_required(VERSION 3.22.1)

project(cxxgame)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
include(ExternalProject)


# =========================================================
# SDL2
# =========================================================

FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz
)

FetchContent_MakeAvailable(SDL2)



# =========================================================
# SDL2_image
# =========================================================

FetchContent_Declare(
    SDL2_image
    URL https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.1/SDL2_image-2.8.1.tar.gz
)

FetchContent_MakeAvailable(SDL2_image)



# =========================================================
# Game
# =========================================================

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)



# =========================================================
# OpenSSL
# =========================================================

if(NOT ANDROID_ABI)
    message(FATAL_ERROR "ANDROID_ABI missing")
endif()


if(ANDROID_ABI STREQUAL "arm64-v8a")

    set(OPENSSL_TARGET android-arm64)
    set(OPENSSL_TRIPLE aarch64-linux-android)

elseif(ANDROID_ABI STREQUAL "armeabi-v7a")

    set(OPENSSL_TARGET android-arm)
    set(OPENSSL_TRIPLE armv7a-linux-androideabi)

elseif(ANDROID_ABI STREQUAL "x86")

    set(OPENSSL_TARGET android-x86)
    set(OPENSSL_TRIPLE i686-linux-android)

elseif(ANDROID_ABI STREQUAL "x86_64")

    set(OPENSSL_TARGET android-x86_64)
    set(OPENSSL_TRIPLE x86_64-linux-android)

else()

    message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI}")

endif()



set(OPENSSL_SOURCE
    ${CMAKE_BINARY_DIR}/openssl-source
)


set(OPENSSL_ROOT
    ${CMAKE_BINARY_DIR}/openssl/${ANDROID_ABI}/install
)


set(OPENSSL_INCLUDE_DIR
    ${OPENSSL_ROOT}/include
)


set(OPENSSL_SSL
    ${OPENSSL_ROOT}/lib/libssl.a
)


set(OPENSSL_CRYPTO
    ${OPENSSL_ROOT}/lib/libcrypto.a
)



# =========================================================
# OpenSSL Build
# =========================================================

ExternalProject_Add(

    openssl_build

    GIT_REPOSITORY
    https://github.com/openssl/openssl.git

    GIT_TAG
    OpenSSL_1_1_1w


    SOURCE_DIR
    ${OPENSSL_SOURCE}


    CONFIGURE_COMMAND

    ${CMAKE_COMMAND} -E env

    PATH=${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}

    CC=${OPENSSL_TRIPLE}23-clang

    CXX=${OPENSSL_TRIPLE}23-clang++

    AR=${OPENSSL_TRIPLE}-ar

    RANLIB=${OPENSSL_TRIPLE}-ranlib

    ANDROID_NDK_ROOT=${ANDROID_NDK}

    ${CMAKE_COMMAND} -E chdir

    ${OPENSSL_SOURCE}

    perl Configure

    ${OPENSSL_TARGET}

    -D__ANDROID_API__=23

    no-shared
    no-async
    no-tests
    no-engine
    no-comp

    --prefix=${OPENSSL_ROOT}



    BUILD_COMMAND

    ${CMAKE_COMMAND} -E chdir

    ${OPENSSL_SOURCE}

    make -j4



    INSTALL_COMMAND

    ${CMAKE_COMMAND} -E chdir

    ${OPENSSL_SOURCE}

    make install_sw



    BUILD_BYPRODUCTS

    ${OPENSSL_SSL}

    ${OPENSSL_CRYPTO}

)



# =========================================================
# Imported OpenSSL
# =========================================================

add_library(
    openssl_crypto
    STATIC
    IMPORTED
)

set_target_properties(
    openssl_crypto
    PROPERTIES

    IMPORTED_LOCATION
    ${OPENSSL_CRYPTO}
)



add_library(
    openssl_ssl
    STATIC
    IMPORTED
)


set_target_properties(
    openssl_ssl
    PROPERTIES

    IMPORTED_LOCATION
    ${OPENSSL_SSL}

    INTERFACE_LINK_LIBRARIES
    openssl_crypto
)



add_dependencies(
    openssl_crypto
    openssl_build
)


add_dependencies(
    openssl_ssl
    openssl_build
)


add_dependencies(
    cxxgame
    openssl_build
)



# =========================================================
# Includes
# =========================================================

target_include_directories(
    cxxgame

    PRIVATE

    ${OPENSSL_INCLUDE_DIR}

    ${SDL2_SOURCE_DIR}
    ${SDL2_SOURCE_DIR}/include

    ${SDL2_image_SOURCE_DIR}
    ${SDL2_image_SOURCE_DIR}/include
)



# =========================================================
# Defines
# =========================================================

target_compile_definitions(
    cxxgame

    PRIVATE

    GAME80_FORCE_OPENSSL_TLS=1

    OPENSSL_API_COMPAT=0x10100000L
)



# =========================================================
# Android Libraries
# =========================================================

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(DL_LIB dl)
find_library(Z_LIB z)
find_library(M_LIB m)



# =========================================================
# Link
# =========================================================

target_link_libraries(

    cxxgame

    SDL2::SDL2

    SDL2_image::SDL2_image

    openssl_ssl

    openssl_crypto

    ${Z_LIB}

    ${LOG_LIB}

    ${ANDROID_LIB}

    ${DL_LIB}

    ${M_LIB}

    c++_shared

)



message(STATUS "==============================")
message(STATUS "Game57 Android Build")
message(STATUS "ABI ${ANDROID_ABI}")
message(STATUS "OpenSSL 1.1.1w static")
message(STATUS "==============================")
