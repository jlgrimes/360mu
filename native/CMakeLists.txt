# 360μ Native Core - CMake Build Configuration
cmake_minimum_required(VERSION 3.22)
project(x360mu_core VERSION 0.1.0 LANGUAGES C CXX ASM)

# C++20 requirement
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(X360MU_BUILD_TESTS "Build unit tests" ON)
option(X360MU_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(X360MU_ENABLE_JIT "Enable JIT compiler (ARM64 only)" ON)
option(X360MU_USE_FFMPEG "Use FFmpeg for XMA decoding" ON)

# Detect Android NDK build
if(ANDROID)
    message(STATUS "Building for Android - ABI: ${ANDROID_ABI}")
    set(X360MU_PLATFORM "android")
    
    # Only enable JIT on ARM64
    if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
        set(X360MU_ENABLE_JIT OFF)
        message(WARNING "JIT disabled: only supported on arm64-v8a")
    endif()
else()
    set(X360MU_PLATFORM "host")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

if(X360MU_ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
endif()

# ARM64 NEON optimization
if(ANDROID_ABI STREQUAL "arm64-v8a" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+simd")
endif()

# Source files organized by module
set(CORE_SOURCES
    src/core/emulator.cpp
)

set(CPU_SOURCES
    src/cpu/xenon/cpu.cpp
    src/cpu/xenon/decoder.cpp
    src/cpu/xenon/interpreter.cpp
    src/cpu/xenon/interpreter_extended.cpp
    src/cpu/xenon/threading.cpp
    src/cpu/vmx128/vmx.cpp
)

if(X360MU_ENABLE_JIT)
    list(APPEND CPU_SOURCES
        src/cpu/jit/jit_compiler.cpp
        src/cpu/jit/arm64_emitter.cpp
        src/cpu/jit/block_cache.cpp
    )
    add_definitions(-DX360MU_JIT_ENABLED)
endif()

# GPU sources - use stubs when Vulkan is not available
if(X360MU_USE_VULKAN)
    set(GPU_SOURCES
        src/gpu/xenos/gpu.cpp
        src/gpu/xenos/command_processor.cpp
        src/gpu/xenos/shader_translator.cpp
        src/gpu/xenos/spirv_builder.cpp
        src/gpu/xenos/edram.cpp
        src/gpu/xenos/texture.cpp
        src/gpu/vulkan/vulkan_backend.cpp
        src/gpu/vulkan/memory_manager.cpp
        src/gpu/vulkan/swapchain.cpp
        src/gpu/shader_cache.cpp
        src/gpu/texture_cache.cpp
        src/gpu/descriptor_manager.cpp
        src/gpu/render_target.cpp
    )
else()
    set(GPU_SOURCES
        src/gpu/stub/gpu_stub.cpp
    )
endif()

# APU sources - full implementation with XMA decoder, mixer, and Android audio output
set(APU_SOURCES
    src/apu/apu.cpp
    src/apu/xma_decoder.cpp
    src/apu/android_audio.cpp
)

set(KERNEL_SOURCES
    src/kernel/kernel.cpp
    src/kernel/threading.cpp
    src/kernel/xobject.cpp
    src/kernel/xthread.cpp
    src/kernel/xevent.cpp
    src/kernel/xkernel.cpp
    src/kernel/hle/xboxkrnl.cpp
    src/kernel/hle/xboxkrnl_extended.cpp
    src/kernel/hle/xboxkrnl_io.cpp
    src/kernel/hle/xboxkrnl_threading.cpp
    src/kernel/hle/xam.cpp
    src/kernel/xex_loader.cpp
    src/kernel/xex_crypto.cpp
    src/kernel/filesystem/vfs.cpp
    src/kernel/filesystem/iso_device.cpp
    src/kernel/filesystem/stfs_device.cpp
)

set(MEMORY_SOURCES
    src/memory/memory.cpp
)

set(JNI_SOURCES
    src/jni/jni_bridge.cpp
)

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${CPU_SOURCES}
    ${GPU_SOURCES}
    ${APU_SOURCES}
    ${KERNEL_SOURCES}
    ${MEMORY_SOURCES}
)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Third-party dependencies
# VMA (Vulkan Memory Allocator) - header only
add_definitions(-DVMA_IMPLEMENTATION)

# Find Vulkan (optional for testing)
option(X360MU_USE_VULKAN "Enable Vulkan rendering" ON)

if(X360MU_USE_VULKAN)
    if(ANDROID)
        # Android has Vulkan in NDK
        find_library(VULKAN_LIB vulkan)
    else()
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            set(VULKAN_LIB Vulkan::Vulkan)
            add_definitions(-DX360MU_HAS_VULKAN)
        else()
            message(WARNING "Vulkan not found - GPU emulation disabled")
            set(X360MU_USE_VULKAN OFF)
        endif()
    endif()
endif()

# Find FFmpeg (optional)
if(X360MU_USE_FFMPEG)
    # For Android, we'll use prebuilt FFmpeg
    if(ANDROID)
        # Expect prebuilt in third_party/ffmpeg/android
        set(FFMPEG_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/include)
        set(FFMPEG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/android/${ANDROID_ABI})
        include_directories(${FFMPEG_INCLUDE_DIR})
        link_directories(${FFMPEG_LIB_DIR})
        set(FFMPEG_LIBS avcodec avformat avutil swresample)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswresample)
        include_directories(${FFMPEG_INCLUDE_DIRS})
        link_directories(${FFMPEG_LIBRARY_DIRS})
        set(FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    endif()
    add_definitions(-DX360MU_USE_FFMPEG)
endif()

# Main library
add_library(x360mu_core SHARED ${ALL_SOURCES})

target_link_libraries(x360mu_core
    ${FFMPEG_LIBS}
)

if(X360MU_USE_VULKAN AND VULKAN_LIB)
    target_link_libraries(x360mu_core ${VULKAN_LIB})
endif()

if(ANDROID)
    # Android-specific libraries
    find_library(LOG_LIB log)
    find_library(ANDROID_LIB android)
    find_library(AAUDIO_LIB aaudio)
    
    target_link_libraries(x360mu_core
        ${LOG_LIB}
        ${ANDROID_LIB}
        ${AAUDIO_LIB}
    )
    
    # JNI bridge - sources already defined above
    add_library(x360mu_jni SHARED ${JNI_SOURCES})
    target_link_libraries(x360mu_jni x360mu_core ${LOG_LIB})
else()
    # Host build - link pthread
    find_package(Threads REQUIRED)
    target_link_libraries(x360mu_core Threads::Threads)
endif()

# Unit tests
if(X360MU_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    
    # Find or fetch Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    
    # JIT tests only on ARM64
    set(JIT_TEST_SOURCES "")
    if(X360MU_ENABLE_JIT AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        set(JIT_TEST_SOURCES tests/cpu/test_jit.cpp)
    endif()
    
    # GPU/Vulkan tests only when Vulkan is available
    set(GPU_TEST_SOURCES "")
    if(X360MU_USE_VULKAN)
        # Shader translator tests (no Vulkan runtime required)
        set(GPU_TEST_SOURCES 
            tests/gpu/test_shader_translator.cpp
            tests/gpu/test_command_processor.cpp
        )
        if(Vulkan_FOUND)
            # Full Vulkan tests require Vulkan SDK
            list(APPEND GPU_TEST_SOURCES tests/gpu/test_vulkan.cpp)
        endif()
    endif()
    
    add_executable(x360mu_tests
        tests/test_main.cpp
        tests/cpu/test_decoder.cpp
        tests/cpu/test_interpreter.cpp
        tests/cpu/test_vmx128.cpp
        tests/memory/test_memory.cpp
        tests/kernel/test_xex_loader.cpp
        tests/kernel/test_filesystem.cpp
        tests/kernel/test_file_io.cpp
        tests/kernel/test_threading.cpp
        tests/kernel/test_xobject.cpp
        tests/kernel/test_xevent.cpp
        tests/kernel/test_xthread.cpp
        tests/kernel/test_xkernel.cpp
        tests/apu/test_audio.cpp
        ${JIT_TEST_SOURCES}
        ${GPU_TEST_SOURCES}
    )
    
    target_link_libraries(x360mu_tests
        x360mu_core
        gtest
        gtest_main
    )
    
    # Link Vulkan for tests if available
    if(X360MU_USE_VULKAN AND Vulkan_FOUND)
        target_link_libraries(x360mu_tests ${VULKAN_LIB})
    endif()
    
    # Disable gtest_discover_tests as it can timeout on some platforms
    # include(GoogleTest)
    # gtest_discover_tests(x360mu_tests)
    
    # XEX Test Tool
    add_executable(xex_test
        tools/xex_test.cpp
    )
    target_link_libraries(xex_test x360mu_core)
    
    # =========================================
    # Integration Test Tools
    # =========================================
    
    # Level 1: ISO Mount Test
    add_executable(test_iso
        tools/test_iso.cpp
    )
    target_link_libraries(test_iso x360mu_core)
    
    add_executable(extract_xex
        tools/extract_xex.cpp
    )
    target_link_libraries(extract_xex x360mu_core)
    
    # Level 2: XEX Load Test
    add_executable(test_xex_load
        tools/test_xex_load.cpp
    )
    target_link_libraries(test_xex_load x360mu_core)
    
    # Level 3: Execution Test
    add_executable(test_execute
        tools/test_execute.cpp
    )
    target_link_libraries(test_execute x360mu_core)
    
    # Level 4: Syscall Trace
    add_executable(test_syscalls
        tools/test_syscalls.cpp
    )
    target_link_libraries(test_syscalls x360mu_core)
endif()

# Install rules
install(TARGETS x360mu_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

if(ANDROID)
    install(TARGETS x360mu_jni
        LIBRARY DESTINATION lib
    )
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "=== 360μ Build Configuration ===")
message(STATUS "Platform:       ${X360MU_PLATFORM}")
message(STATUS "Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "JIT enabled:    ${X360MU_ENABLE_JIT}")
message(STATUS "Vulkan:         ${X360MU_USE_VULKAN}")
message(STATUS "FFmpeg:         ${X360MU_USE_FFMPEG}")
message(STATUS "Tests:          ${X360MU_BUILD_TESTS}")
message(STATUS "ASAN:           ${X360MU_ENABLE_ASAN}")
message(STATUS "================================")
message(STATUS "")

