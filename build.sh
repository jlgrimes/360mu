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

resolve_cmake() {
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake
        return 0
    fi

    # Try Android SDK bundled CMake first.
    local sdk_roots=("${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk")
    local root
    for root in "${sdk_roots[@]}"; do
        [ -n "$root" ] || continue
        if [ -d "$root/cmake" ]; then
            local candidate
            candidate=$(find "$root/cmake" -maxdepth 3 -type f -name cmake 2>/dev/null | sort -Vr | head -n 1)
            if [ -n "$candidate" ] && [ -x "$candidate" ]; then
                echo "$candidate"
                return 0
            fi
        fi
    done

    # Try Python package shim if already installed.
    if python3 -m cmake --version >/dev/null 2>&1; then
        echo "python3 -m cmake"
        return 0
    fi

    # Last resort: bootstrap cmake in a local project virtualenv.
    if command -v python3 >/dev/null 2>&1; then
        local tool_venv="${PROJECT_ROOT}/.tooling/cmake-venv"
        local venv_cmake="${tool_venv}/bin/cmake"

        if [ ! -x "$venv_cmake" ]; then
            echo -e "${YELLOW}CMake not found; bootstrapping local tool venv at ${tool_venv}...${NC}" >&2
            mkdir -p "${PROJECT_ROOT}/.tooling"
            if python3 -m venv "$tool_venv" >/dev/null 2>&1 \
                && "$tool_venv/bin/pip" install --quiet --upgrade pip >/dev/null 2>&1 \
                && "$tool_venv/bin/pip" install --quiet cmake >/dev/null 2>&1; then
                :
            else
                echo -e "${YELLOW}Local cmake bootstrap failed (venv/pip install).${NC}" >&2
            fi
        fi

        if [ -x "$venv_cmake" ]; then
            echo "$venv_cmake"
            return 0
        fi
    fi

    return 1
}

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
        --profile)
            BUILD_TYPE="Profile"
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
            echo "Usage: $0 [--release] [--profile] [--test] [--clean]"
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

if ! CMAKE_BIN=$(resolve_cmake); then
    echo -e "${RED}Failed to locate CMake. Install cmake or set ANDROID_SDK_ROOT/ANDROID_HOME with sdk/cmake present.${NC}"
    exit 1
fi

# Support command values that may include spaces (e.g. "python3 -m cmake")
read -r -a CMAKE_CMD <<< "$CMAKE_BIN"

echo -e "${GREEN}Using CMake:${NC} ${CMAKE_CMD[*]}"
echo -e "${GREEN}Configuring CMake (${BUILD_TYPE})...${NC}"

# Configure CMake
"${CMAKE_CMD[@]}" .. \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DX360MU_BUILD_TESTS=ON \
    -DX360MU_ENABLE_JIT=OFF \
    -DX360MU_USE_FFMPEG=OFF \
    -DX360MU_USE_VULKAN=OFF

echo ""
echo -e "${GREEN}Building...${NC}"

# Build with all cores
"${CMAKE_CMD[@]}" --build . --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

BUILD_RESULT=$?

if [ $BUILD_RESULT -eq 0 ]; then
    echo ""
    echo -e "${GREEN}Build successful!${NC}"

    # Symlink compile_commands.json to project root for IDE support
    if [ -f "${BUILD_DIR}/compile_commands.json" ]; then
        ln -sf "${BUILD_DIR}/compile_commands.json" "${PROJECT_ROOT}/compile_commands.json"
        echo -e "${GREEN}compile_commands.json linked to project root${NC}"
    fi

    # Run tests if requested
    if [ "$RUN_TESTS" = true ]; then
        echo ""
        echo -e "${GREEN}Running tests...${NC}"

        CTEST_BIN=""
        if command -v ctest >/dev/null 2>&1; then
            CTEST_BIN=$(command -v ctest)
        elif [ -x "$(dirname "${CMAKE_CMD[0]}")/ctest" ]; then
            CTEST_BIN="$(dirname "${CMAKE_CMD[0]}")/ctest"
        fi

        if [ -n "$CTEST_BIN" ]; then
            "$CTEST_BIN" --output-on-failure
        elif [ -x "${BUILD_DIR}/x360mu_tests" ]; then
            "${BUILD_DIR}/x360mu_tests"
        else
            echo -e "${YELLOW}No ctest/x360mu_tests binary found; build completed but tests were not executed.${NC}"
            exit 1
        fi
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
