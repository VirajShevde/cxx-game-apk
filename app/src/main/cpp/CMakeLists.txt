# =========================================================
# OpenSSL 1.1.1w Android setup
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

    message(FATAL_ERROR "Unsupported ABI: ${ANDROID_ABI}")

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
# Build OpenSSL 1.1.1w
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

    ANDROID_NDK_ROOT=${ANDROID_NDK}

    ANDROID_API=23


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

    ${CMAKE_COMMAND} -E env

    PATH=${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}

    ${CMAKE_COMMAND} -E chdir

    ${OPENSSL_SOURCE}

    make -j4



    INSTALL_COMMAND

    ${CMAKE_COMMAND} -E env

    PATH=${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:$ENV{PATH}

    ${CMAKE_COMMAND} -E chdir

    ${OPENSSL_SOURCE}

    make install_sw



    BUILD_BYPRODUCTS

    ${OPENSSL_SSL}

    ${OPENSSL_CRYPTO}

)
