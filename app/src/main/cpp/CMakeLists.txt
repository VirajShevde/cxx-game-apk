cmake_minimum_required(VERSION 3.22.1)

project(cxxgame)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================================
# FetchContent
# =========================================================

include(FetchContent)


# =========================================================
# SDL2 2.30.12
# =========================================================

FetchContent_Declare(
    SDL2
    URL
    https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
)

FetchContent_MakeAvailable(SDL2)


# =========================================================
# SDL2_image 2.8.1
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


set(
    OPENSSL_ROOT
    ${CMAKE_SOURCE_DIR}/openssl/${ANDROID_ABI}
)


set(
    OPENSSL_INCLUDE_DIR
    ${OPENSSL_ROOT}/include
)


set(
    OPENSSL_SSL
    ${OPENSSL_ROOT}/lib/libssl.a
)


set(
    OPENSSL_CRYPTO
    ${OPENSSL_ROOT}/lib/libcrypto.a
)


# =========================================================
# Verify OpenSSL files
# =========================================================

if(NOT EXISTS ${OPENSSL_INCLUDE_DIR}/openssl/ssl.h)
    message(FATAL_ERROR
        "Missing OpenSSL headers: ${OPENSSL_INCLUDE_DIR}"
    )
endif()


if(NOT EXISTS ${OPENSSL_INCLUDE_DIR}/openssl/rsa.h)
    message(FATAL_ERROR
        "Missing OpenSSL RSA header: ${OPENSSL_INCLUDE_DIR}/openssl/rsa.h"
    )
endif()


if(NOT EXISTS ${OPENSSL_INCLUDE_DIR}/openssl/bn.h)
    message(FATAL_ERROR
        "Missing OpenSSL BIGNUM header: ${OPENSSL_INCLUDE_DIR}/openssl/bn.h"
    )
endif()


if(NOT EXISTS ${OPENSSL_SSL})
    message(FATAL_ERROR
        "Missing OpenSSL ssl library: ${OPENSSL_SSL}"
    )
endif()


if(NOT EXISTS ${OPENSSL_CRYPTO})
    message(FATAL_ERROR
        "Missing OpenSSL crypto library: ${OPENSSL_CRYPTO}"
    )
endif()


# =========================================================
# Imported OpenSSL Crypto
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


# =========================================================
# Imported OpenSSL SSL
# =========================================================

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


# =========================================================
# Include Directories
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
# Compile Definitions
# =========================================================

target_compile_definitions(
    cxxgame
    PRIVATE

    GAME80_FORCE_OPENSSL_TLS=1
)


# =========================================================
# Android Libraries
# =========================================================

find_library(
    LOG_LIB
    log
)

find_library(
    ANDROID_LIB
    android
)

find_library(
    DL_LIB
    dl
)

find_library(
    Z_LIB
    z
)

find_library(
    M_LIB
    m
)


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


# =========================================================
# Status
# =========================================================

message(STATUS "================================")
message(STATUS "Game57 Android Build")
message(STATUS "ABI: ${ANDROID_ABI}")
message(STATUS "SDL2 2.30.12 enabled")
message(STATUS "SDL2_image 2.8.1 enabled")
message(STATUS "OpenSSL 1.1.1w enabled")
message(STATUS "Static OpenSSL enabled")
message(STATUS "OpenSSL headers verified")
message(STATUS "================================")
