#!/bin/bash
# 360μ Build Script

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/native/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 360μ Xbox 360 Emulator Build ===${NC}"
echo ""

# Parse arguments
BUILD_TYPE="Debug"
RUN_TESTS=false
CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --test)
            RUN_TESTS=true
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--release] [--test] [--clean]"
            exit 1
            ;;
    esac
done

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo -e "${GREEN}Configuring CMake (${BUILD_TYPE})...${NC}"

# Configure CMake
cmake .. \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DX360MU_BUILD_TESTS=ON \
    -DX360MU_ENABLE_JIT=OFF \
    -DX360MU_USE_FFMPEG=OFF \
    -DX360MU_USE_VULKAN=OFF

echo ""
echo -e "${GREEN}Building...${NC}"

# Build with all cores
cmake --build . --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

BUILD_RESULT=$?

if [ $BUILD_RESULT -eq 0 ]; then
    echo ""
    echo -e "${GREEN}Build successful!${NC}"
    
    # Run tests if requested
    if [ "$RUN_TESTS" = true ]; then
        echo ""
        echo -e "${GREEN}Running tests...${NC}"
        ctest --output-on-failure
    fi
else
    echo ""
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}Build artifacts:${NC}"
ls -la "${BUILD_DIR}/"lib* 2>/dev/null || true
ls -la "${BUILD_DIR}/"x360mu* 2>/dev/null || true

