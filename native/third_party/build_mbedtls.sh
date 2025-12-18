#!/bin/bash
# Build mbedTLS 3.x for Android
# Run this script from the third_party directory

set -e

# Using 3.5.0 - last version that doesn't require git submodules for framework
MBEDTLS_VERSION="3.5.0"
MBEDTLS_DIR="mbedtls-${MBEDTLS_VERSION}"
MBEDTLS_TAR="v${MBEDTLS_VERSION}.tar.gz"

# Check for Android NDK
if [ -z "$ANDROID_NDK_HOME" ]; then
    # Try common locations
    if [ -d "$HOME/Library/Android/sdk/ndk/27.0.12077973" ]; then
        export ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/27.0.12077973"
    elif [ -d "$HOME/Library/Android/sdk/ndk-bundle" ]; then
        export ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk-bundle"
    elif [ -d "$HOME/Android/Sdk/ndk/27.0.12077973" ]; then
        export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/27.0.12077973"
    else
        echo "Error: ANDROID_NDK_HOME not set and NDK not found in common locations"
        echo "Please set ANDROID_NDK_HOME to your Android NDK path"
        exit 1
    fi
fi

echo "Using Android NDK: $ANDROID_NDK_HOME"

# Download mbedTLS if not present
if [ ! -d "$MBEDTLS_DIR" ]; then
    echo "Downloading mbedTLS ${MBEDTLS_VERSION}..."
    curl -L "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/${MBEDTLS_TAR}" -o "${MBEDTLS_TAR}"
    tar xzf "${MBEDTLS_TAR}"
    rm "${MBEDTLS_TAR}"
fi

# Create output directories
mkdir -p mbedtls/include
mkdir -p mbedtls/android/arm64-v8a
mkdir -p mbedtls/android/armeabi-v7a
mkdir -p mbedtls/android/x86_64
mkdir -p mbedtls/android/x86

# Copy headers
echo "Copying headers..."
cp -r "${MBEDTLS_DIR}/include/mbedtls" mbedtls/include/
cp -r "${MBEDTLS_DIR}/include/psa" mbedtls/include/

# Disable AESNI (x86 hardware acceleration) for Android - causes build issues
echo "Disabling AESNI for Android compatibility..."
sed -i.bak 's/^#define MBEDTLS_AESNI_C/\/\/ #define MBEDTLS_AESNI_C/' "${MBEDTLS_DIR}/include/mbedtls/mbedtls_config.h"
# Also disable in our copied headers
sed -i.bak 's/^#define MBEDTLS_AESNI_C/\/\/ #define MBEDTLS_AESNI_C/' "mbedtls/include/mbedtls/mbedtls_config.h"

# Build function
build_for_abi() {
    local ABI=$1
    local ARCH=$2
    local API_LEVEL=21
    
    echo ""
    echo "=========================================="
    echo "Building mbedTLS for ${ABI}..."
    echo "=========================================="
    
    local BUILD_DIR="${MBEDTLS_DIR}/build-${ABI}"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM="android-${API_LEVEL}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTING=OFF \
        -DENABLE_PROGRAMS=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF \
        -DCMAKE_INSTALL_PREFIX="../install-${ABI}"
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) mbedcrypto
    
    cd ../..
    
    # Copy the built library
    cp "${MBEDTLS_DIR}/build-${ABI}/library/libmbedcrypto.a" "mbedtls/android/${ABI}/"
    
    echo "Built libmbedcrypto.a for ${ABI}"
}

# Build for each ABI
build_for_abi "arm64-v8a" "aarch64"
build_for_abi "armeabi-v7a" "arm"
build_for_abi "x86_64" "x86_64"
build_for_abi "x86" "i686"

echo ""
echo "=========================================="
echo "mbedTLS build complete!"
echo "=========================================="
echo ""
echo "Libraries installed to:"
echo "  - mbedtls/include/          (headers)"
echo "  - mbedtls/android/arm64-v8a/"
echo "  - mbedtls/android/armeabi-v7a/"
echo "  - mbedtls/android/x86_64/"
echo "  - mbedtls/android/x86/"
echo ""
echo "You can now build the Android app."
