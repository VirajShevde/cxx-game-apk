cmake_minimum_required(VERSION 3.22.1)

project("cxxgame")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# =========================================================
# SDL2
# =========================================================

FetchContent_Declare(
    SDL2
    URL "https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz"
    URL_HASH SHA256=332cb37d0be20cb9541739c61f79bae5a477427d79ae85e352089afdaf6666e4
)

FetchContent_MakeAvailable(SDL2)

message(STATUS "SDL2 source directory: ${SDL2_SOURCE_DIR}")
message(STATUS "SDL2 include directory: ${SDL2_SOURCE_DIR}/include")


# =========================================================
# SDL2_image
# =========================================================

FetchContent_Declare(
    SDL2_image
    URL "https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.1/SDL2_image-2.8.1.tar.gz"
    URL_HASH SHA256=e4cab9a58c347a490c46723c17553b4e12233cd821d3b993a8475a50497f5a3e
)

FetchContent_MakeAvailable(SDL2_image)


# =========================================================
# YOUR SOURCE
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
    message(FATAL_ERROR
        "ANDROID_ABI is not defined. Use the Android NDK."
    )
endif()


set(OPENSSL_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/openssl/${ANDROID_ABI}"
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


if(NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/ssl.h")
    message(FATAL_ERROR
        "OpenSSL headers missing:\n${OPENSSL_INCLUDE_DIR}"
    )
endif()


if(NOT EXISTS "${OPENSSL_SSL}")
    message(FATAL_ERROR
        "libssl.a missing:\n${OPENSSL_SSL}"
    )
endif()


if(NOT EXISTS "${OPENSSL_CRYPTO}")
    message(FATAL_ERROR
        "libcrypto.a missing:\n${OPENSSL_CRYPTO}"
    )
endif()


add_library(openssl_crypto STATIC IMPORTED)

set_target_properties(openssl_crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)


add_library(openssl_ssl STATIC IMPORTED)

set_target_properties(openssl_ssl PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES openssl_crypto
)


# =========================================================
# Include paths
# =========================================================

target_include_directories(cxxgame PRIVATE

    # OpenSSL
    "${OPENSSL_INCLUDE_DIR}"

    # SDL2
    "${SDL2_SOURCE_DIR}"
    "${SDL2_SOURCE_DIR}/include"

    # SDL2_image
    "${SDL2_IMAGE_SOURCE_DIR}"
    "${SDL2_IMAGE_SOURCE_DIR}/include"
)


# =========================================================
# Compile definitions
# =========================================================

target_compile_definitions(cxxgame PRIVATE
    GAME80_FORCE_OPENSSL_TLS=1
)


# =========================================================
# Android libraries
# =========================================================

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(DL_LIB dl)
find_library(Z_LIB z)


# =========================================================
# Link
# =========================================================

target_link_libraries(cxxgame

    SDL2::SDL2
    SDL2_image::SDL2_image

    openssl_ssl
    openssl_crypto

    ${LOG_LIB}
    ${ANDROID_LIB}
    ${DL_LIB}
    ${Z_LIB}
)


message(STATUS "========================================")
message(STATUS "Game57 Android Build")
message(STATUS "Source: game57-4.cxx")
message(STATUS "ABI: ${ANDROID_ABI}")
message(STATUS "SDL2: ENABLED")
message(STATUS "SDL2_image: ENABLED")
message(STATUS "OpenSSL TLS: ENABLED")
message(STATUS "========================================")
