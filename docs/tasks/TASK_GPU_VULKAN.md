# Task: GPU/Vulkan Backend Implementation

## Project Context

You are working on 360μ, an Xbox 360 emulator for Android. The project uses C++20 and Vulkan for rendering.

## Your Assignment

Implement the Vulkan rendering backend that translates Xbox 360 Xenos GPU commands to Vulkan.

## Current State

- Stub implementation exists at `native/src/gpu/stub/gpu_stub.cpp`
- Headers defined at `native/src/gpu/xenos/gpu.h`
- Vulkan backend header at `native/src/gpu/vulkan/vulkan_backend.h`
- Shader translator header at `native/src/gpu/xenos/shader_translator.h`

## Files to Implement

### 1. `native/src/gpu/vulkan/vulkan_backend.cpp`

```cpp
// Implement:
- VulkanBackend::initialize() - Create Vulkan instance, device, swapchain
- VulkanBackend::create_swapchain() - Handle Android surface
- VulkanBackend::begin_frame() / end_frame()
- VulkanBackend::create_pipeline() - From translated shaders
- VulkanBackend::bind_vertex_buffers()
- VulkanBackend::draw_indexed()
- VulkanBackend::resolve_render_target() - eDRAM to texture
```

### 2. `native/src/gpu/xenos/shader_translator.cpp`

```cpp
// Implement Xenos microcode → SPIR-V translation:
- Parse Xenos shader control flow (cf_exec, cf_loop, cf_jump)
- Translate ALU instructions (scalar: MUL, ADD, etc; vector: DOT, CROSS)
- Translate TEX fetch instructions
- Handle Xbox 360 specific: register indexing, predication
- Output valid SPIR-V binary
```

### 3. `native/src/gpu/xenos/command_processor.cpp`

```cpp
// Implement PM4 packet parsing:
- Type 0: Register writes
- Type 3: Draw commands (DRAW_INDEX, DRAW_AUTO)
- SET_CONSTANT packets
- Handle ring buffer read/write pointers
```

### 4. `native/src/gpu/xenos/edram.cpp`

```cpp
// Implement 10MB eDRAM emulation:
- Tile-based rendering (32x32 tiles)
- Morton/Z-order addressing
- MSAA resolve (2x, 4x)
- Format conversion (Xbox formats → Vulkan)
```

## Key Xenos GPU Facts

- 10MB eDRAM for render targets (tiled, Morton order)
- 48 shader ALUs (unified vertex/pixel)
- Shader microcode format (not HLSL bytecode)
- Register-based constant storage
- PM4 command packets from ATI R500

## Reference Materials

- Xenia GPU source: https://github.com/xenia-project/xenia/tree/master/src/xenia/gpu
- Xenos shader ISA: Search "Xenos shader microcode" in Xenia wiki

## Build & Test

```bash
# Enable Vulkan in build:
cd native/build
cmake .. -DX360MU_USE_VULKAN=ON
make -j4

# Test (create test that renders triangle):
./x360mu_tests --gtest_filter=GPU*
```

## Dependencies

- Vulkan SDK must be installed
- Android NDK for mobile build
- No dependency on other tasks (CPU/Audio)

## Success Criteria

1. Can create Vulkan swapchain on Android
2. Can translate simple Xenos shaders to SPIR-V
3. Can render a solid color to screen
4. Can process basic PM4 draw packets
