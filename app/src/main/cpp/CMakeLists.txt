cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
include(ExternalProject)


# =========================================================
# SDL2
# =========================================================

FetchContent_Declare(
    SDL2
    URL
    https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz
)

FetchContent_MakeAvailable(SDL2)



# =========================================================
# SDL2_image
# =========================================================

FetchContent_Declare(
    SDL2_image
    URL
    https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.1/SDL2_image-2.8.1.tar.gz
)

FetchContent_MakeAvailable(SDL2_image)



# =========================================================
# Game Library
# =========================================================

add_library(
    cxxgame
    SHARED
    game57-4.cxx
)



# =========================================================
# OpenSSL 1.1.1w
# =========================================================

if(NOT ANDROID_ABI)
    message(FATAL_ERROR "ANDROID_ABI missing")
endif()


if(ANDROID_ABI STREQUAL "arm64-v8a")

    set(OPENSSL_TARGET android-arm64)

elseif(ANDROID_ABI STREQUAL "armeabi-v7a")

    set(OPENSSL_TARGET android-arm)

elseif(ANDROID_ABI STREQUAL "x86")

    set(OPENSSL_TARGET android-x86)

elseif(ANDROID_ABI STREQUAL "x86_64")

    set(OPENSSL_TARGET android-x86_64)

else()

    message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI}")

endif()



set(OPENSSL_ROOT
    "${CMAKE_BINARY_DIR}/openssl/${ANDROID_ABI}/install"
)

set(OPENSSL_SOURCE
    "${CMAKE_BINARY_DIR}/openssl-source"
)

set(OPENSSL_INCLUDE_DIR
    "${OPENSSL_ROOT}/include"
)

set(OPENSSL_LIB_DIR
    "${OPENSSL_ROOT}/lib"
)

set(OPENSSL_SSL
    "${OPENSSL_LIB_DIR}/libssl.a"
)

set(OPENSSL_CRYPTO
    "${OPENSSL_LIB_DIR}/libcrypto.a"
)



ExternalProject_Add(

    openssl_build

    GIT_REPOSITORY
    https://github.com/openssl/openssl.git

    GIT_TAG
    OpenSSL_1_1_1w


    SOURCE_DIR
    ${OPENSSL_SOURCE}


    CONFIGURE_COMMAND

    perl ./Configure
    ${OPENSSL_TARGET}
    -D__ANDROID_API__=23
    no-shared
    no-async
    no-tests
    no-engine
    no-comp
    --prefix=${OPENSSL_ROOT}


    BUILD_COMMAND

    make -j4


    INSTALL_COMMAND

    make install_sw


    BUILD_BYPRODUCTS

    ${OPENSSL_SSL}
    ${OPENSSL_CRYPTO}
)



# =========================================================
# OpenSSL Libraries
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

    INTERFACE_INCLUDE_DIRECTORIES
    ${OPENSSL_INCLUDE_DIR}
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

    INTERFACE_INCLUDE_DIRECTORIES
    ${OPENSSL_INCLUDE_DIR}

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
# Include directories
# =========================================================

target_include_directories(
    cxxgame PRIVATE

    ${OPENSSL_INCLUDE_DIR}

    ${SDL2_SOURCE_DIR}
    ${SDL2_SOURCE_DIR}/include

    ${SDL2_image_SOURCE_DIR}
    ${SDL2_image_SOURCE_DIR}/include
)



# =========================================================
# Compile options
# =========================================================

target_compile_definitions(
    cxxgame PRIVATE

    GAME80_FORCE_OPENSSL_TLS=1

    OPENSSL_API_COMPAT=0x10100000L
)



# =========================================================
# Android system libraries
# =========================================================

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(DL_LIB dl)
find_library(Z_LIB z)
find_library(M_LIB m)



# =========================================================
# Link libraries
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



# =========================================================
# Status
# =========================================================

message(
    STATUS
    "================================"
)

message(
    STATUS
    "Game57 Android Build"
)

message(
    STATUS
    "ABI: ${ANDROID_ABI}"
)

message(
    STATUS
    "SDL2 enabled"
)

message(
    STATUS
    "SDL2_image enabled"
)

message(
    STATUS
    "OpenSSL 1.1.1w enabled"
)

message(
    STATUS
    "================================"
)
