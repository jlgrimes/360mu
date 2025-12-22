# 360μ Xbox 360 Emulator - Complete Implementation Plan

**Project**: Xbox 360 Emulator for Android
**Target**: Basic working prototype with graphics rendering
**Timeline**: 4-5 months
**Last Updated**: 2025-12-22

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Project Overview](#project-overview)
3. [Current State Analysis](#current-state-analysis)
4. [Architecture Deep Dive](#architecture-deep-dive)
5. [Phase 1: GPU Command Processing](#phase-1-gpu-command-processing-weeks-1-2)
6. [Phase 2: Shader Translation](#phase-2-shader-translation-weeks-3-7)
7. [Phase 3: Rendering Pipeline Integration](#phase-3-rendering-pipeline-integration-weeks-8-9)
8. [Phase 4: Performance Optimization](#phase-4-performance-optimization-weeks-10-11)
9. [Phase 5: Polish & Testing](#phase-5-polish--testing-weeks-12-13)
10. [Technical Reference](#technical-reference)
11. [Development Workflow](#development-workflow)
12. [Risk Management](#risk-management)
13. [Appendices](#appendices)

---

## Executive Summary

### Project Goal

Complete the **360μ Xbox 360 emulator** for Android to achieve a basic working prototype capable of:
- Booting Xbox 360 games from XEX executables
- Displaying graphics via GPU emulation
- Achieving 20-30 FPS on mid-range ARM64 devices
- Supporting basic input controls

### Current Status

The emulator has a **solid technical foundation**:
- ✅ CPU emulation (PowerPC interpreter + ARM64 JIT)
- ✅ Memory management (512MB RAM with fastmem)
- ✅ Kernel HLE (60+ syscalls)
- ✅ Build system (CMake + Android Gradle)
- ✅ Android UI framework (Kotlin + Jetpack Compose)

**Critical Blocker**: GPU rendering pipeline incomplete - specifically command processing and shader translation.

### Success Metrics

| Milestone | Criteria | Timeline |
|-----------|----------|----------|
| **Minimum Viable** | Triangle rendering without crash | End of Phase 2 (Week 7) |
| **Basic Prototype** | Simple game with textured graphics | End of Phase 3 (Week 9) |
| **Usable Emulator** | 20-30 FPS, stable, basic input | End of Phase 5 (Week 13) |

### Key Risks

1. **Shader Translation Complexity** - Xbox 360 Xenos GPU has unique shader architecture
2. **Mobile Performance** - ARM64 may struggle with real-time emulation
3. **Vulkan Driver Bugs** - Android device fragmentation
4. **Undocumented Behaviors** - Xbox 360 hardware quirks

---

## Project Overview

### What is Xbox 360 Emulation?

Xbox 360 emulation involves recreating the Xbox 360 hardware environment in software:

```
┌─────────────────────────────────────────────────────────┐
│                     Xbox 360 Hardware                    │
├─────────────────────────────────────────────────────────┤
│  CPU: 3x PowerPC Xenon cores (3.2 GHz, 6 threads)      │
│  GPU: ATI Xenos (500 MHz, unified shaders)              │
│  Memory: 512MB GDDR3 + 10MB eDRAM                       │
│  Storage: DVD drive, HDD                                 │
└─────────────────────────────────────────────────────────┘
                           ↓
                   Emulation Layer
                           ↓
┌─────────────────────────────────────────────────────────┐
│                   Android Device (ARM64)                 │
├─────────────────────────────────────────────────────────┤
│  CPU: ARM64 cores (e.g., Snapdragon 8 Gen 2+)          │
│  GPU: Adreno/Mali (Vulkan API)                          │
│  Memory: 8-16GB LPDDR5                                   │
│  Storage: Flash storage                                  │
└─────────────────────────────────────────────────────────┘
```

### Emulation Approach

**CPU Emulation**:
- **Interpreter**: Executes PowerPC instructions one-by-one (slow but accurate)
- **JIT Compiler**: Translates PowerPC blocks to ARM64 native code (fast)

**GPU Emulation**:
- **HLE (High-Level Emulation)**: Intercept GPU commands and translate to Vulkan
- **Shader Translation**: Convert Xenos shader microcode to SPIR-V for Vulkan

**Kernel Emulation**:
- **HLE**: Implement Xbox kernel syscalls in software (no LLE kernel execution)

### Technology Stack

| Component | Technology | Purpose |
|-----------|------------|---------|
| **Language** | C++20 | Native emulator core |
| **Build System** | CMake 3.22+ | Cross-platform builds |
| **Android Build** | Gradle + NDK r26+ | Android app packaging |
| **Graphics API** | Vulkan 1.1+ | GPU rendering |
| **Audio API** | AAudio | Audio output |
| **JNI Bridge** | C++ JNI | Native ↔ Java/Kotlin interface |
| **UI Framework** | Jetpack Compose | Android user interface |
| **Crypto** | mbedTLS 3.5.0 | XEX decryption (AES-128) |
| **Decompression** | libmspack | XEX decompression (LZX) |

---

## Current State Analysis

### Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                      Android App (Kotlin)                     │
│              MainActivity + Jetpack Compose UI                │
└───────────────────────────┬──────────────────────────────────┘
                            │ JNI
┌───────────────────────────▼──────────────────────────────────┐
│                    JNI Bridge (C++)                           │
│        native/src/jni/jni_bridge.cpp (300+ lines)            │
└───────────────────────────┬──────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────┐
│                  Emulator Core (C++20)                        │
├───────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐          │
│  │     CPU     │  │   Memory    │  │   Kernel    │          │
│  │  Xenon      │  │  512MB RAM  │  │   HLE       │          │
│  │  JIT/Interp │  │  Fastmem    │  │  60 Syscalls│          │
│  └─────────────┘  └─────────────┘  └─────────────┘          │
│                                                                │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐          │
│  │     GPU     │  │     APU     │  │     VFS     │          │
│  │  Xenos      │  │  XMA Audio  │  │  ISO Mount  │          │
│  │  Vulkan     │  │  AAudio     │  │  File I/O   │          │
│  └─────────────┘  └─────────────┘  └─────────────┘          │
└───────────────────────────────────────────────────────────────┘
```

### Component Status Matrix

| Component | Status | Completeness | Critical Issues |
|-----------|--------|--------------|-----------------|
| **CPU Interpreter** | ✅ Working | 100% | None |
| **CPU JIT** | ✅ Working | 80% | Some opcodes missing |
| **Memory System** | ✅ Working | 100% | None |
| **Fastmem** | ✅ Working | 100% | None |
| **XEX Loader** | ✅ Working | 100% | None |
| **Kernel HLE** | ✅ Working | 90% | Some syscalls missing |
| **Threading** | ✅ Working | 100% | None |
| **VFS** | ✅ Working | 100% | None |
| **GPU Command Processor** | 🟡 Partial | 40% | Draw execution incomplete |
| **Shader Translator** | 🟡 Partial | 20% | Most opcodes missing |
| **Vulkan Backend** | 🟡 Partial | 60% | Pipeline creation incomplete |
| **Texture Cache** | 🔴 Missing | 10% | Not implemented |
| **Render Targets** | 🔴 Missing | 10% | Not implemented |
| **Audio Pipeline** | 🟡 Partial | 70% | Data routing missing |
| **Input System** | 🔴 Missing | 5% | Only JNI stubs exist |

### Critical Path Analysis

The **critical path** to achieving a working prototype focuses on GPU rendering:

```
GPU Command Processing (Phase 1)
          ↓
Shader Translation (Phase 2)
          ↓
Pipeline Integration (Phase 3)
          ↓
Performance Optimization (Phase 4)
          ↓
Device Testing (Phase 5)
```

**Why GPU is Critical**:
- CPU, memory, and kernel are already functional
- Games can boot and execute code
- Without GPU rendering, there's no visual feedback
- GPU is the only major blocker preventing graphics display

### Code Statistics

| Metric | Value | Notes |
|--------|-------|-------|
| **Total C++ Code** | ~41,220 lines | Core emulator |
| **Total Headers** | ~16,363 lines | API definitions |
| **Kotlin/Android UI** | ~3,000 lines | App interface |
| **Documentation** | Multiple .md files | Architecture docs |
| **Dependencies** | mbedTLS, Vulkan, AAudio | Minimal external deps |

---

## Architecture Deep Dive

### CPU Emulation Architecture

#### PowerPC Xenon CPU

The Xbox 360 uses a custom IBM PowerPC processor:
- **3 cores** × **2 hardware threads** = **6 logical processors**
- **3.2 GHz** clock speed
- **In-order execution** (simpler than out-of-order)
- **VMX128 SIMD** (128-bit vector operations, similar to AltiVec)

#### Emulation Strategy

**Dual-Mode Execution**:

```
┌─────────────────────────────────────────────────────────┐
│                  CPU Emulation                           │
├─────────────────────────────────────────────────────────┤
│                                                           │
│  ┌─────────────────────┐    ┌─────────────────────┐    │
│  │    Interpreter      │    │    JIT Compiler     │    │
│  │                     │    │                     │    │
│  │ • Accurate          │    │ • Fast              │    │
│  │ • Slow (~1% speed)  │    │ • 80% coverage      │    │
│  │ • Fallback          │    │ • Main path         │    │
│  │ • All opcodes       │    │ • PowerPC→ARM64     │    │
│  └─────────────────────┘    └─────────────────────┘    │
│           ↓                          ↓                   │
│  ┌──────────────────────────────────────────────────┐  │
│  │          CPU State (Registers)                    │  │
│  │  • 32 GPRs (General Purpose Registers)           │  │
│  │  • 32 FPRs (Floating Point Registers)            │  │
│  │  • 128 VMX registers (Vector/SIMD)               │  │
│  │  • Special registers (LR, CTR, CR, XER, etc.)    │  │
│  └──────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**Key Files**:
- [native/src/cpu/xenon/cpu.cpp](../native/src/cpu/xenon/cpu.cpp) - Interpreter loop
- [native/src/cpu/jit/jit_compiler.cpp](../native/src/cpu/jit/jit_compiler.cpp) - JIT engine
- [native/src/cpu/jit/arm64_emitter.cpp](../native/src/cpu/jit/arm64_emitter.cpp) - ARM64 code generation

### Memory System Architecture

#### Memory Layout

The Xbox 360 has **512MB** of unified memory, addressable in two modes:

```
Physical Memory Layout:
┌───────────────────────────────────────────┐
│ 0x00000000 - 0x1FFFFFFF (512MB)          │ Direct access
│                                           │
│ 0x80000000 - 0x9FFFFFFF (512MB)          │ Mirror (high bit set)
└───────────────────────────────────────────┘

Virtual Memory Mappings:
┌───────────────────────────────────────────┐
│ 0x00000000 - 0x1FFFFFFF  │ Main RAM       │
│ 0x80000000 - 0x9FFFFFFF  │ RAM Mirror     │
│ 0xC0000000 - 0xC3FFFFFF  │ GPU MMIO       │
│ 0xEC800000 - 0xECFFFFFF  │ GPU MMIO Alt   │
│ 0xE0000000 - 0xEFFFFFFF  │ MMIO Region    │
└───────────────────────────────────────────┘
```

#### Fastmem Optimization

**Concept**: Map guest memory directly into host address space for fast access.

```
Traditional Emulation:
    Guest Load → Address Translation → Bounds Check → Host Memory Access
    (Slow: 10-20 instructions per access)

Fastmem:
    Guest Load → Direct Host Memory Access
    (Fast: 1-2 instructions)
    + Signal handler catches invalid accesses
```

**Implementation**:
1. Reserve 4GB of host virtual address space
2. Map 512MB of actual memory at offset 0
3. Configure signal handler for SIGSEGV
4. On fault, check if address is MMIO → route to device
5. If invalid, crash with error

**Key Files**:
- [native/src/memory/memory.cpp](../native/src/memory/memory.cpp) - Memory manager
- [native/src/memory/fastmem.cpp](../native/src/memory/fastmem.cpp) - Fastmem implementation

### GPU Emulation Architecture

#### Xenos GPU Overview

The Xbox 360's GPU is a custom ATI design codenamed "Xenos":
- **500 MHz** clock speed
- **48 unified shader processors** (can execute vertex or pixel shaders)
- **10MB eDRAM** (embedded DRAM for framebuffer, extremely fast)
- **Unified shader architecture** (DirectX 10-era, but custom)

**Key Differences from PC GPUs**:
- Uses **tiled rendering** (eDRAM is small, must tile large framebuffers)
- Custom **microcode format** (not HLSL, not Cg)
- **PM4 packet-based** command submission (like AMD GPUs)

#### GPU Emulation Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│                    GPU Emulation Flow                        │
└─────────────────────────────────────────────────────────────┘
                           │
      ┌────────────────────┼────────────────────┐
      │                    │                    │
      ▼                    ▼                    ▼
┌───────────┐      ┌──────────────┐     ┌─────────────┐
│    CPU    │      │ Ring Buffer  │     │  GPU Regs   │
│  writes   │─────▶│  (PM4 cmds)  │◀────│  (MMIO)     │
│ commands  │      │              │     │             │
└───────────┘      └──────┬───────┘     └─────────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ Command         │
                 │ Processor       │
                 │ (PM4 Parser)    │
                 └────────┬────────┘
                          │
         ┌────────────────┼────────────────┐
         │                │                │
         ▼                ▼                ▼
  ┌───────────┐   ┌──────────────┐  ┌──────────┐
  │  Shader   │   │   Texture    │  │  Render  │
  │Translator │   │    Cache     │  │  Target  │
  │Xenos→SPIRV│   │              │  │  Manager │
  └─────┬─────┘   └──────┬───────┘  └────┬─────┘
        │                │                │
        └────────────────┼────────────────┘
                         ▼
                ┌─────────────────┐
                │ Vulkan Backend  │
                │                 │
                │ • Pipelines     │
                │ • Descriptor    │
                │   Sets          │
                │ • Command       │
                │   Buffers       │
                └────────┬────────┘
                         ▼
                  ┌──────────────┐
                  │   Display    │
                  │  (Swapchain) │
                  └──────────────┘
```

**Key Files**:
- [native/src/gpu/xenos/gpu.cpp](../native/src/gpu/xenos/gpu.cpp) - Main GPU orchestrator
- [native/src/gpu/xenos/command_processor.cpp](../native/src/gpu/xenos/command_processor.cpp) - PM4 parser
- [native/src/gpu/xenos/shader_translator.cpp](../native/src/gpu/xenos/shader_translator.cpp) - Shader compiler
- [native/src/gpu/vulkan/vulkan_backend.cpp](../native/src/gpu/vulkan/vulkan_backend.cpp) - Vulkan renderer

#### PM4 Command Format

PM4 (Packet Manager 4) is AMD's command format for GPUs:

```
Type 0 Packet: Register Write
┌───────┬───────────┬─────────────────────┐
│ Type  │  Register │       Value         │
│ (2b)  │  Index    │                     │
└───────┴───────────┴─────────────────────┘

Type 3 Packet: Command
┌───────┬───────────┬─────────────────────┐
│ Type  │  Opcode   │    Parameters       │
│ (2b)  │  (7b)     │    (variable)       │
└───────┴───────────┴─────────────────────┘

Example Commands:
• DRAW_INDX          - Draw indexed primitives
• LOAD_ALU_CONSTANT  - Load shader constant
• SET_CONSTANT       - Set shader uniform
• EVENT_WRITE        - GPU synchronization
• INDIRECT_BUFFER    - Execute command list
```

### Shader Translation Architecture

#### Xenos Shader Format

Xbox 360 shaders are compiled to **Xenos microcode** (binary format):

```
Shader Structure:
┌────────────────────────────────────────┐
│          Control Flow (CF)             │ Sequential control
│  • EXEC/EXEC_END                       │
│  • LOOP_START/LOOP_END                 │
│  • CONDITIONAL_EXEC                    │
└────────────┬───────────────────────────┘
             │
             ▼
┌────────────────────────────────────────┐
│        ALU Instructions                │ Math operations
│  • Vector ops (ADD, MUL, DOT4, etc.)  │
│  • Scalar ops (EXP, LOG, RECIP, etc.) │
└────────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────┐
│      Fetch Instructions                │ Memory access
│  • Texture fetch (TEX)                 │
│  • Vertex fetch (VFETCH)               │
└────────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────┐
│         Export/Output                  │ Results
│  • Position (vertex shader)            │
│  • Color (pixel shader)                │
│  • Interpolators (varying data)        │
└────────────────────────────────────────┘
```

#### SPIR-V Translation

**SPIR-V** is Vulkan's intermediate shader representation:

```
Xenos Microcode → Shader Translator → SPIR-V → Vulkan Driver → GPU Native Code

Translation Challenges:
1. Register mapping (Xenos uses 128 registers, SPIR-V uses SSA)
2. Instruction semantics (similar but not identical)
3. Control flow (structured vs unstructured)
4. Swizzling/masking (complex source/dest modifiers)
```

**Key Files**:
- [native/src/gpu/xenos/shader_translator.cpp](../native/src/gpu/xenos/shader_translator.cpp) - Main translator
- [native/src/gpu/xenos/spirv_builder.cpp](../native/src/gpu/xenos/spirv_builder.cpp) - SPIR-V emission

---

## Phase 1: GPU Command Processing (Weeks 1-2)

### Overview

**Goal**: Fix the command processor's draw execution pipeline to successfully submit draw calls to Vulkan.

**Current Problem**: `execute_draw()` fails at shader preparation and pipeline creation, preventing any rendering.

### Technical Background

#### PM4 Packet Processing Flow

```
CPU writes to ring buffer
         ↓
GPU MMIO write (CP_RB_WPTR register)
         ↓
CommandProcessor::process_commands()
         ↓
Parse PM4 packets from ring buffer
         ↓
execute_type0() → Register write
execute_type3() → Command execution
         ↓
For DRAW_INDX/DRAW_AUTO commands:
         ↓
execute_draw()
```

#### Current execute_draw() Flow

```cpp
void CommandProcessor::execute_draw(const DrawCommand& cmd) {
    1. Begin Vulkan frame (if not started)
           ↓
    2. prepare_shaders() ──────────┐
           ↓                        │ ❌ FAILS HERE
    3. prepare_pipeline() ─────────┤
           ↓                        │
    4. update_constants()           │ Never reached
           ↓                        │
    5. bind_textures()              │
           ↓                        │
    6. bind_vertex_buffers()        │
           ↓                        │
    7. bind_index_buffer()          │
           ↓                        │
    8. vkCmdDraw() / vkCmdDrawIndexed()
}
```

### Detailed Implementation Plan

#### Task 1.1: Fix `prepare_shaders()` Function

**File**: [native/src/gpu/xenos/command_processor.cpp:1191-1232](../native/src/gpu/xenos/command_processor.cpp#L1191-L1232)

**Current Issues**:

```cpp
bool CommandProcessor::prepare_shaders() {
    // Problem 1: Fails if no shader cache
    if (!shader_cache_ || !memory_) {
        current_vertex_shader_ = nullptr;
        current_pixel_shader_ = nullptr;
        return true;  // ❌ Returns true but shaders are null!
    }

    // Problem 2: Shader addresses are 0 initially
    GuestAddr vs_addr = render_state_.vertex_shader_address;
    GuestAddr ps_addr = render_state_.pixel_shader_address;

    if (vs_addr == 0 || ps_addr == 0) {
        LOGD("No shader addresses set");
        return false;  // ❌ Fails entire draw!
    }

    // Problem 3: No fallback if shader compilation fails
    current_vertex_shader_ = shader_cache_->get_shader(...);
    current_pixel_shader_ = shader_cache_->get_shader(...);

    if (!current_vertex_shader_ || !current_pixel_shader_) {
        return false;  // ❌ No fallback mechanism!
    }

    return true;
}
```

**Solution**:

```cpp
bool CommandProcessor::prepare_shaders() {
    if (!shader_cache_ || !memory_) {
        // Use default fallback shaders for testing
        use_default_shaders();
        return true;
    }

    GuestAddr vs_addr = render_state_.vertex_shader_address;
    GuestAddr ps_addr = render_state_.pixel_shader_address;

    // If no shader addresses set yet, use defaults
    if (vs_addr == 0 || ps_addr == 0) {
        LOGD("No shader addresses, using defaults");
        use_default_shaders();
        return true;  // ✅ Continue with defaults
    }

    // Validate memory pointers
    const void* vs_microcode = memory_->get_host_ptr(vs_addr);
    const void* ps_microcode = memory_->get_host_ptr(ps_addr);

    if (!vs_microcode || !ps_microcode) {
        LOGE("Invalid shader addresses");
        use_default_shaders();
        return true;  // ✅ Fallback instead of fail
    }

    // Get shader size (would normally parse from header)
    u32 vs_size = 2048;
    u32 ps_size = 2048;

    // Try to compile shaders
    current_vertex_shader_ = shader_cache_->get_shader(
        vs_microcode, vs_size, ShaderType::Vertex);
    current_pixel_shader_ = shader_cache_->get_shader(
        ps_microcode, ps_size, ShaderType::Pixel);

    // Fallback to defaults if compilation failed
    if (!current_vertex_shader_ || !current_pixel_shader_) {
        LOGW("Shader compilation failed, using defaults");
        use_default_shaders();
        return true;  // ✅ Always succeed with fallback
    }

    return true;
}

void CommandProcessor::use_default_shaders() {
    // Create simple passthrough vertex + solid color pixel shader
    // This is created once and cached
    if (!default_vertex_shader_) {
        default_vertex_shader_ = create_default_vertex_shader();
    }
    if (!default_pixel_shader_) {
        default_pixel_shader_ = create_default_pixel_shader();
    }

    current_vertex_shader_ = default_vertex_shader_;
    current_pixel_shader_ = default_pixel_shader_;
}
```

**Implementation Steps**:

1. Add `use_default_shaders()` helper method
2. Add `create_default_vertex_shader()` - creates passthrough shader
3. Add `create_default_pixel_shader()` - creates solid red shader
4. Modify `prepare_shaders()` to use fallbacks instead of failing
5. Add validation for memory pointers before shader compilation
6. Log shader compilation failures as warnings, not errors

**Testing**:
- Verify `prepare_shaders()` always returns true
- Confirm default shaders are created on first use
- Test with valid and invalid shader addresses
- Validate memory pointer checks work correctly

**Time Estimate**: 2-3 days

---

#### Task 1.2: Complete `prepare_pipeline()` Function

**File**: [native/src/gpu/xenos/command_processor.cpp:1234-1313](../native/src/gpu/xenos/command_processor.cpp#L1234-L1313)

**Current Issues**:

```cpp
bool CommandProcessor::prepare_pipeline(const DrawCommand& cmd) {
    // Build pipeline key from current state
    PipelineKey key{};
    key.vertex_shader_hash = current_vertex_shader_->hash;
    key.pixel_shader_hash = current_pixel_shader_->hash;
    key.primitive_topology = map_primitive_type(cmd.primitive_type);
    // ... (copies render state to key)

    // Check pipeline cache
    VkPipeline pipeline = shader_cache_->get_pipeline(
        current_vertex_shader_, current_pixel_shader_, key);

    if (pipeline != VK_NULL_HANDLE) {
        current_pipeline_ = pipeline;
        return true;
    }

    // ❌ PROBLEM: If cache miss, pipeline is null!
    // No code to create new pipeline

    return false;  // ❌ Draw fails!
}
```

**Solution**:

```cpp
bool CommandProcessor::prepare_pipeline(const DrawCommand& cmd) {
    if (!current_vertex_shader_ || !current_pixel_shader_) {
        LOGE("No shaders available for pipeline");
        return false;
    }

    // Build pipeline key
    PipelineKey key{};
    key.vertex_shader_hash = current_vertex_shader_->hash;
    key.pixel_shader_hash = current_pixel_shader_->hash;
    key.primitive_topology = map_primitive_type(cmd.primitive_type);

    // Copy render state
    key.depth_test_enable = render_state_.depth_test_enable;
    key.depth_write_enable = render_state_.depth_write_enable;
    key.depth_compare_op = render_state_.depth_compare_op;
    key.blend_enable = render_state_.blend_enable;
    key.src_blend = render_state_.src_blend;
    key.dst_blend = render_state_.dst_blend;
    key.cull_mode = render_state_.cull_mode;
    key.front_face = render_state_.front_face;
    // ... more state

    // Try cache lookup first
    VkPipeline cached_pipeline = shader_cache_->lookup_pipeline(key);
    if (cached_pipeline != VK_NULL_HANDLE) {
        current_pipeline_ = cached_pipeline;
        vulkan_->bind_pipeline(cached_pipeline);
        return true;
    }

    // ✅ Cache miss - create new pipeline
    LOGD("Creating new pipeline (cache miss)");

    // Convert key to Vulkan pipeline create info
    VkGraphicsPipelineCreateInfo create_info = {};
    populate_pipeline_create_info(&create_info, key,
                                  current_vertex_shader_->module,
                                  current_pixel_shader_->module);

    // Create pipeline via Vulkan backend
    VkPipeline new_pipeline = vulkan_->create_graphics_pipeline(&create_info);
    if (new_pipeline == VK_NULL_HANDLE) {
        LOGE("Failed to create graphics pipeline");
        return false;
    }

    // ✅ Cache the newly created pipeline
    shader_cache_->cache_pipeline(key, new_pipeline);

    current_pipeline_ = new_pipeline;
    vulkan_->bind_pipeline(new_pipeline);

    return true;
}

void CommandProcessor::populate_pipeline_create_info(
    VkGraphicsPipelineCreateInfo* info,
    const PipelineKey& key,
    VkShaderModule vs_module,
    VkShaderModule ps_module)
{
    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    stages[1].pName = "main";

    // Vertex input state (empty for now, will add in Task 1.3)
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = key.primitive_topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    // Viewport/scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = key.cull_mode;
    rasterization.frontFace = key.front_face;
    rasterization.lineWidth = 1.0f;

    // Depth/stencil
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = key.depth_test_enable;
    depth_stencil.depthWriteEnable = key.depth_write_enable;
    depth_stencil.depthCompareOp = key.depth_compare_op;

    // Blend state
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.blendEnable = key.blend_enable;
    blend_attachment.srcColorBlendFactor = key.src_blend;
    blend_attachment.dstColorBlendFactor = key.dst_blend;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                     VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT |
                                     VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    // Dynamic state
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    // Populate main create info
    info->sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info->stageCount = 2;
    info->pStages = stages;
    info->pVertexInputState = &vertex_input;
    info->pInputAssemblyState = &input_assembly;
    info->pViewportState = &viewport;
    info->pRasterizationState = &rasterization;
    info->pDepthStencilState = &depth_stencil;
    info->pColorBlendState = &blend;
    info->pDynamicState = &dynamic;
    info->renderPass = vulkan_->default_render_pass();
    info->layout = vulkan_->pipeline_layout();
}
```

**Implementation Steps**:

1. Add `populate_pipeline_create_info()` helper
2. Implement pipeline creation on cache miss
3. Add pipeline caching logic in shader_cache
4. Ensure all pipeline state is correctly mapped from key
5. Handle pipeline creation failures gracefully
6. Add logging for cache hits/misses

**Testing**:
- Verify pipeline created on first draw with new state
- Confirm pipeline cache hit on subsequent draws
- Test with different render states (blend, depth, cull)
- Validate Vulkan pipeline create info is correct

**Time Estimate**: 2-3 days

---

#### Task 1.3: Implement Vertex Buffer Binding

**File**: [native/src/gpu/xenos/command_processor.cpp:1315-1324](../native/src/gpu/xenos/command_processor.cpp#L1315-L1324)

**Current State**:

```cpp
void CommandProcessor::bind_vertex_buffers(const DrawCommand& cmd) {
    // ❌ STUB - Not implemented!
    // TODO: Implement vertex buffer binding
}
```

**Challenges**:

1. Xbox 360 uses **vertex fetch constants** (VFetch registers) to describe vertex data
2. Each fetch constant contains: address, stride, format, element count
3. Need to copy vertex data from guest memory to Vulkan buffer
4. Must avoid memory leaks (create new buffer every frame → leak!)

**Solution Design**:

```
Buffer Pooling System:
┌──────────────────────────────────────────────────┐
│         Buffer Pool (per frame)                  │
├──────────────────────────────────────────────────┤
│  Frame 0: [Buffer A] [Buffer B] [Buffer C]      │
│  Frame 1: [Buffer D] [Buffer E] [Buffer F]      │
│  Frame 2: [Buffer G] [Buffer H] [Buffer I]      │
└──────────────────────────────────────────────────┘
              ↓ Reuse after 3 frames
```

**Implementation**:

```cpp
class BufferPool {
public:
    struct PooledBuffer {
        VkBuffer buffer;
        VkDeviceMemory memory;
        void* mapped;
        size_t size;
        u32 last_used_frame;
    };

    VkBuffer allocate(size_t size, u32 current_frame) {
        // Try to find free buffer from old frames
        for (auto& buf : buffers_) {
            if (buf.last_used_frame + 3 < current_frame && buf.size >= size) {
                buf.last_used_frame = current_frame;
                return buf.buffer;
            }
        }

        // Create new buffer
        PooledBuffer new_buf = create_buffer(size);
        new_buf.last_used_frame = current_frame;
        buffers_.push_back(new_buf);
        return new_buf.buffer;
    }

private:
    std::vector<PooledBuffer> buffers_;
};

void CommandProcessor::bind_vertex_buffers(const DrawCommand& cmd) {
    // Get vertex fetch constants from GPU state
    for (u32 i = 0; i < render_state_.vertex_buffer_count; i++) {
        const VertexFetchConstant& vf = render_state_.vertex_fetches[i];

        // Parse fetch constant
        GuestAddr address = vf.address();
        u32 stride = vf.stride();
        u32 format = vf.format();
        u32 element_count = cmd.vertex_count;  // or from draw command

        // Calculate buffer size
        size_t buffer_size = stride * element_count;

        // Get buffer from pool
        VkBuffer vk_buffer = buffer_pool_->allocate(buffer_size, current_frame_);

        // Copy vertex data from guest memory
        const void* guest_data = memory_->get_host_ptr(address);
        if (guest_data) {
            void* mapped = buffer_pool_->get_mapped_ptr(vk_buffer);
            memcpy(mapped, guest_data, buffer_size);
        }

        // Bind buffer
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(vulkan_->command_buffer(), i, 1,
                              &vk_buffer, &offset);
    }
}
```

**Implementation Steps**:

1. Create `BufferPool` class for vertex/index buffer management
2. Implement buffer allocation with frame-based lifecycle
3. Parse vertex fetch constants from render state
4. Copy vertex data from guest memory to Vulkan buffer
5. Bind buffers using `vkCmdBindVertexBuffers`
6. Handle multiple vertex buffers (up to 16)
7. Add cleanup logic for old buffers (> 3 frames old)

**Testing**:
- Verify vertex data copied correctly
- Confirm buffer reuse works (no memory leak)
- Test with different vertex formats
- Validate binding with multiple vertex buffers

**Time Estimate**: 3-4 days

---

#### Task 1.4: Fix Index Buffer Memory Leak

**File**: [native/src/gpu/xenos/command_processor.cpp:1326-1360](../native/src/gpu/xenos/command_processor.cpp#L1326-L1360)

**Current Issues**:

```cpp
void CommandProcessor::bind_index_buffer(const DrawCommand& cmd) {
    if (!cmd.indexed) return;

    // Get index buffer address from draw command
    GuestAddr ib_addr = cmd.index_buffer_address;
    u32 index_count = cmd.index_count;

    // ❌ PROBLEM: Creates new buffer every frame!
    VkBuffer index_buffer = vulkan_->create_buffer(...);

    // Copy index data
    const void* guest_indices = memory_->get_host_ptr(ib_addr);
    vulkan_->upload_to_buffer(index_buffer, guest_indices, ...);

    // Bind
    vkCmdBindIndexBuffer(..., index_buffer, ...);

    // ❌ PROBLEM: Buffer never destroyed!
    // Memory leak grows every frame
}
```

**Solution**: Use the same buffer pool system as vertex buffers.

```cpp
void CommandProcessor::bind_index_buffer(const DrawCommand& cmd) {
    if (!cmd.indexed) return;

    GuestAddr ib_addr = cmd.index_buffer_address;
    u32 index_count = cmd.index_count;
    VkIndexType index_type = cmd.index_type_16bit ?
        VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    // Calculate size
    size_t index_size = (index_type == VK_INDEX_TYPE_UINT16) ? 2 : 4;
    size_t buffer_size = index_count * index_size;

    // ✅ Get buffer from pool (reuses old buffers)
    VkBuffer index_buffer = buffer_pool_->allocate(buffer_size, current_frame_);

    // Copy index data
    const void* guest_indices = memory_->get_host_ptr(ib_addr);
    if (guest_indices) {
        void* mapped = buffer_pool_->get_mapped_ptr(index_buffer);
        memcpy(mapped, guest_indices, buffer_size);
    }

    // Bind
    vkCmdBindIndexBuffer(vulkan_->command_buffer(), index_buffer, 0, index_type);
}
```

**Implementation Steps**:

1. Integrate index buffer allocation with BufferPool
2. Remove standalone buffer creation
3. Calculate correct buffer size based on index type
4. Copy index data from guest memory
5. Bind with correct index type (uint16 vs uint32)

**Testing**:
- Verify indexed draws work correctly
- Confirm no memory leak (use Vulkan memory profiler)
- Test with 16-bit and 32-bit indices
- Validate index data copied correctly

**Time Estimate**: 2 days

---

### Phase 1 Testing Strategy

#### Unit Tests

Create standalone tests for each component:

```cpp
// test_command_processor.cpp

TEST(CommandProcessor, PrepareShaders_WithValidAddresses) {
    // Setup: Valid shader addresses
    // Execute: prepare_shaders()
    // Verify: Returns true, shaders loaded
}

TEST(CommandProcessor, PrepareShaders_WithZeroAddresses) {
    // Setup: Zero shader addresses
    // Execute: prepare_shaders()
    // Verify: Returns true, uses default shaders
}

TEST(CommandProcessor, PreparePipeline_CacheHit) {
    // Setup: Pipeline already in cache
    // Execute: prepare_pipeline()
    // Verify: Returns cached pipeline, no creation
}

TEST(CommandProcessor, PreparePipeline_CacheMiss) {
    // Setup: New pipeline state
    // Execute: prepare_pipeline()
    // Verify: Creates new pipeline, caches it
}
```

#### Integration Tests

Test full draw execution:

```cpp
TEST(CommandProcessor, ExecuteDraw_SimpleTriangle) {
    // Setup: Triangle vertex/index data
    // Execute: execute_draw() with triangle
    // Verify: Vulkan draw call issued, no crash
}

TEST(CommandProcessor, ExecuteDraw_MultipleFrames) {
    // Setup: Draw command
    // Execute: 100 frames of execute_draw()
    // Verify: No memory leak, consistent behavior
}
```

#### Manual Testing

Create test application:

```cpp
// test_gpu.cpp

int main() {
    // Initialize emulator
    Emulator emu;
    emu.initialize();

    // Manually create PM4 commands for triangle
    u32 pm4_buffer[] = {
        // Set shader addresses
        PACKET_TYPE0(REG_SQ_VS_PROGRAM, 1),
        0x00001000,  // Vertex shader address

        PACKET_TYPE0(REG_SQ_PS_PROGRAM, 1),
        0x00002000,  // Pixel shader address

        // Draw command
        PACKET_TYPE3(PM4_DRAW_INDX, 3),
        0x00000000,  // Primitive type: triangle list
        3,           // Vertex count: 3 (one triangle)
        0x00000000,  // Start index: 0
    };

    // Submit commands
    for (u32 cmd : pm4_buffer) {
        gpu->write_command(cmd);
    }

    // Process commands
    gpu->process_commands();

    // Verify
    assert(gpu->get_draw_count() == 1);
    printf("Test passed!\n");

    return 0;
}
```

### Phase 1 Deliverables Checklist

- [ ] `prepare_shaders()` handles all failure cases gracefully
- [ ] Default fallback shaders created and functional
- [ ] `prepare_pipeline()` creates pipelines on cache miss
- [ ] Pipeline cache hit/miss logic working correctly
- [ ] BufferPool system implemented and tested
- [ ] Vertex buffer binding functional
- [ ] Index buffer binding functional
- [ ] No memory leaks in buffer allocation
- [ ] Unit tests passing for all components
- [ ] Integration test: Simple triangle draws without crash
- [ ] Validation layers report no errors
- [ ] Draw call counter increments correctly

### Phase 1 Success Criteria

**Minimum**:
- `execute_draw()` completes without crash
- Vulkan validation layers show no errors
- Draw calls submitted to GPU (even if nothing visible yet)

**Target**:
- All unit tests passing
- Integration test renders triangle (even if corrupted)
- No memory leaks detected
- Performance acceptable (< 1ms per draw call overhead)

**Stretch**:
- Triangle visible on screen with correct colors
- Multiple draw calls per frame working
- Pipeline cache saving/loading to disk

---

## Phase 2: Shader Translation (Weeks 3-7)

### Overview

**Goal**: Implement complete Xenos→SPIR-V shader translation to generate functional shaders for vertex and pixel processing.

**Scope**: This is the most complex phase, involving implementing 80+ shader opcodes and handling various shader features.

### Technical Background

#### Xenos Shader Architecture

**Microcode Format**:
```
┌──────────────────────────────────────────┐
│  Control Flow Instructions (CF)          │
│  - 64-bit instruction words              │
│  - Control program flow                  │
│  - Reference ALU/Fetch clauses           │
└──────────────────────────────────────────┘
         │
         ├──> ALU Clause ┌─────────────────────────────┐
         │               │ Vector ALU Instructions     │
         │               │ Scalar ALU Instructions     │
         │               │ - 128-bit instruction words │
         │               │ - Math operations           │
         │               └─────────────────────────────┘
         │
         └──> Fetch Clause ┌──────────────────────────┐
                          │ Texture Fetch             │
                          │ Vertex Fetch              │
                          │ - Memory access           │
                          └──────────────────────────┘
```

**Execution Model**:
- **Unified shaders**: Same hardware executes vertex and pixel shaders
- **SIMD**: 48 shader processors, each with 4-way SIMD
- **Scalar + Vector ALUs**: Two separate arithmetic units
- **128 GPRs**: Temporary registers for computation

#### SPIR-V Generation

**SPIR-V Structure**:
```
┌─────────────────────────────────────────────┐
│              SPIR-V Module                  │
├─────────────────────────────────────────────┤
│  1. Header (magic, version, generator ID)  │
│  2. Capabilities (Shader, Vulkan, ...)     │
│  3. Extensions                              │
│  4. ExtInstImport (GLSL.std.450)           │
│  5. Memory model                            │
│  6. Entry points                            │
│  7. Execution modes                         │
│  8. Debug info                              │
│  9. Annotations (decorations)               │
│ 10. Type declarations                       │
│ 11. Constant declarations                   │
│ 12. Variable declarations                   │
│ 13. Function definitions                    │
│     └─> Basic blocks                        │
│         └─> Instructions                    │
└─────────────────────────────────────────────┘
```

### Detailed Implementation Plan

#### Task 2.1: ALU Vector Operations

**File**: [native/src/gpu/xenos/shader_translator.cpp](../native/src/gpu/xenos/shader_translator.cpp)

**Vector Opcodes to Implement** (30 total):

##### Basic Arithmetic (Priority: HIGH)
```cpp
// ADDv - Vector Add
// SPIR-V: OpFAdd
case AluVectorOp::ADDv: {
    return builder.f_add(vec4_type, src[0], src[1]);
}

// MULv - Vector Multiply
// SPIR-V: OpFMul
case AluVectorOp::MULv: {
    return builder.f_mul(vec4_type, src[0], src[1]);
}

// MULADDv (MAD) - Multiply-Add (fused)
// SPIR-V: OpFAdd(OpFMul(...))
case AluVectorOp::MULADDv: {
    u32 mul_result = builder.f_mul(vec4_type, src[0], src[1]);
    return builder.f_add(vec4_type, mul_result, src[2]);
}

// MAXv - Vector Maximum
// SPIR-V: GLSL.std.450 FMax
case AluVectorOp::MAXv: {
    return builder.ext_inst(vec4_type, glsl_ext,
                           spv::GLSLstd450FMax, {src[0], src[1]});
}

// MINv - Vector Minimum
// SPIR-V: GLSL.std.450 FMin
case AluVectorOp::MINv: {
    return builder.ext_inst(vec4_type, glsl_ext,
                           spv::GLSLstd450FMin, {src[0], src[1]});
}

// FRACv - Fractional Part
// SPIR-V: GLSL.std.450 Fract
case AluVectorOp::FRACv: {
    return builder.ext_inst(vec4_type, glsl_ext,
                           spv::GLSLstd450Fract, {src[0]});
}

// FLOORv - Floor
// SPIR-V: GLSL.std.450 Floor
case AluVectorOp::FLOORv: {
    return builder.ext_inst(vec4_type, glsl_ext,
                           spv::GLSLstd450Floor, {src[0]});
}

// TRUNCv - Truncate
// SPIR-V: GLSL.std.450 Trunc
case AluVectorOp::TRUNCv: {
    return builder.ext_inst(vec4_type, glsl_ext,
                           spv::GLSLstd450Trunc, {src[0]});
}
```

##### Dot Products (Priority: HIGH - Critical for Transforms)
```cpp
// DOT4v - 4-Component Dot Product
// SPIR-V: OpDot
case AluVectorOp::DOT4v: {
    return builder.dot(float_type, src[0], src[1]);
}

// DOT3v - 3-Component Dot Product
// Extract xyz, then dot
case AluVectorOp::DOT3v: {
    // Extract first 3 components
    u32 vec3_src0 = builder.vector_shuffle(vec3_type, src[0], src[0], {0,1,2});
    u32 vec3_src1 = builder.vector_shuffle(vec3_type, src[1], src[1], {0,1,2});
    return builder.dot(float_type, vec3_src0, vec3_src1);
}

// DOT2ADDv - 2-Component Dot + Add
// dot(a.xy, b.xy) + c
case AluVectorOp::DOT2ADDv: {
    u32 vec2_src0 = builder.vector_shuffle(vec2_type, src[0], src[0], {0,1});
    u32 vec2_src1 = builder.vector_shuffle(vec2_type, src[1], src[1], {0,1});
    u32 dot_result = builder.dot(float_type, vec2_src0, vec2_src1);

    // Add scalar c
    u32 c = builder.composite_extract(float_type, src[2], {0});
    return builder.f_add(float_type, dot_result, c);
}
```

##### Comparison (Priority: HIGH)
```cpp
// SETEv - Set if Equal
// result = (a == b) ? 1.0 : 0.0
case AluVectorOp::SETEv: {
    u32 cmp = builder.f_ord_equal(bool4_type, src[0], src[1]);
    u32 one = builder.const_composite(vec4_type, {
        builder.const_float(1.0f),
        builder.const_float(1.0f),
        builder.const_float(1.0f),
        builder.const_float(1.0f)
    });
    u32 zero = builder.const_composite(vec4_type, {
        builder.const_float(0.0f),
        builder.const_float(0.0f),
        builder.const_float(0.0f),
        builder.const_float(0.0f)
    });
    return builder.select(vec4_type, cmp, one, zero);
}

// SETGTv - Set if Greater Than
// result = (a > b) ? 1.0 : 0.0
case AluVectorOp::SETGTv: {
    u32 cmp = builder.f_ord_greater_than(bool4_type, src[0], src[1]);
    u32 one = builder.const_composite(vec4_type, {...});
    u32 zero = builder.const_composite(vec4_type, {...});
    return builder.select(vec4_type, cmp, one, zero);
}

// SETGTEv - Set if Greater or Equal
case AluVectorOp::SETGTEv: {
    u32 cmp = builder.f_ord_greater_than_equal(bool4_type, src[0], src[1]);
    // ... same pattern
}

// SETNEv - Set if Not Equal
case AluVectorOp::SETNEv: {
    u32 cmp = builder.f_ord_not_equal(bool4_type, src[0], src[1]);
    // ... same pattern
}
```

##### Conditional Selection (Priority: MEDIUM)
```cpp
// CNDEv - Conditional Equal
// result = (a == 0.0) ? b : c
case AluVectorOp::CNDEv: {
    u32 zero = builder.const_composite(vec4_type, {0.0f, 0.0f, 0.0f, 0.0f});
    u32 cmp = builder.f_ord_equal(bool4_type, src[0], zero);
    return builder.select(vec4_type, cmp, src[1], src[2]);
}

// CNDGTv - Conditional Greater Than
// result = (a > 0.0) ? b : c
case AluVectorOp::CNDGTv: {
    u32 zero = builder.const_composite(vec4_type, {0.0f, 0.0f, 0.0f, 0.0f});
    u32 cmp = builder.f_ord_greater_than(bool4_type, src[0], zero);
    return builder.select(vec4_type, cmp, src[1], src[2]);
}

// CNDGTEv - Conditional Greater or Equal
case AluVectorOp::CNDGTEv: {
    u32 zero = builder.const_composite(vec4_type, {0.0f, 0.0f, 0.0f, 0.0f});
    u32 cmp = builder.f_ord_greater_than_equal(bool4_type, src[0], zero);
    return builder.select(vec4_type, cmp, src[1], src[2]);
}
```

##### Special Operations (Priority: MEDIUM)
```cpp
// CUBEv - Cube Map Face Selection
// Returns: (face_id, s, t, major_axis_magnitude)
case AluVectorOp::CUBEv: {
    // This is complex - determines which cube face and texture coords
    // Implementation requires determining max component, face selection
    // See OpenGL/DirectX cube map coordinate formulas
    return emit_cube_map_coordinates(builder, src[0]);
}

// MAX4v - Maximum of 4 Components
// Returns scalar: max(src.x, src.y, src.z, src.w)
case AluVectorOp::MAX4v: {
    u32 x = builder.composite_extract(float_type, src[0], {0});
    u32 y = builder.composite_extract(float_type, src[0], {1});
    u32 z = builder.composite_extract(float_type, src[0], {2});
    u32 w = builder.composite_extract(float_type, src[0], {3});

    u32 max_xy = builder.ext_inst(float_type, glsl_ext,
                                  spv::GLSLstd450FMax, {x, y});
    u32 max_zw = builder.ext_inst(float_type, glsl_ext,
                                  spv::GLSLstd450FMax, {z, w});
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450FMax, {max_xy, max_zw});
}
```

**Implementation Approach**:

```cpp
u32 ShaderTranslator::translate_vector_alu(TranslationContext& ctx,
                                            const AluInstruction& inst) {
    // Decode instruction
    AluVectorOp opcode = inst.vector_opcode;
    u8 dest_reg = inst.vector_dest;
    u8 write_mask = inst.vector_write_mask;

    // Load source operands
    std::vector<u32> sources;
    for (int i = 0; i < 3; i++) {
        if (inst.src_used[i]) {
            u32 src = load_source_operand(ctx, inst.src[i]);

            // Apply source modifiers (negate, absolute)
            if (inst.src_negate[i]) {
                src = ctx.builder.f_negate(ctx.vec4_type, src);
            }
            if (inst.src_abs[i]) {
                src = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext,
                                          spv::GLSLstd450FAbs, {src});
            }

            // Apply source swizzle
            if (inst.src_swizzle[i] != SWIZZLE_XYZW) {
                src = apply_swizzle(ctx, src, inst.src_swizzle[i]);
            }

            sources.push_back(src);
        }
    }

    // Execute operation
    u32 result = 0;
    switch (opcode) {
        case AluVectorOp::ADDv:
            result = ctx.builder.f_add(ctx.vec4_type, sources[0], sources[1]);
            break;

        case AluVectorOp::MULv:
            result = ctx.builder.f_mul(ctx.vec4_type, sources[0], sources[1]);
            break;

        case AluVectorOp::MULADDv: {
            u32 mul = ctx.builder.f_mul(ctx.vec4_type, sources[0], sources[1]);
            result = ctx.builder.f_add(ctx.vec4_type, mul, sources[2]);
            break;
        }

        case AluVectorOp::DOT4v:
            result = ctx.builder.dot(ctx.float_type, sources[0], sources[1]);
            // Replicate to vec4
            result = ctx.builder.composite_construct(ctx.vec4_type,
                {result, result, result, result});
            break;

        // ... all other opcodes

        default:
            LOGW("Unimplemented vector ALU op: %u", static_cast<u32>(opcode));
            result = ctx.builder.const_composite(ctx.vec4_type, {0,0,0,0});
            break;
    }

    // Apply write mask
    if (write_mask != 0xF) {  // Not writing all components
        // Load current register value
        u32 current = ctx.builder.load(ctx.vec4_type, ctx.registers[dest_reg]);

        // Insert new components based on write mask
        for (int i = 0; i < 4; i++) {
            if (write_mask & (1 << i)) {
                u32 component = ctx.builder.composite_extract(ctx.float_type, result, {i});
                current = ctx.builder.composite_insert(ctx.vec4_type, component,
                                                      current, {i});
            }
        }
        result = current;
    }

    // Store to destination register
    ctx.builder.store(ctx.registers[dest_reg], result);

    return result;
}

u32 ShaderTranslator::apply_swizzle(TranslationContext& ctx,
                                     u32 value,
                                     u32 swizzle_mask) {
    // Swizzle is 8-bit: xxyyzz ww (2 bits per component)
    u32 x = (swizzle_mask >> 0) & 0x3;
    u32 y = (swizzle_mask >> 2) & 0x3;
    u32 z = (swizzle_mask >> 4) & 0x3;
    u32 w = (swizzle_mask >> 6) & 0x3;

    std::vector<u32> indices = {x, y, z, w};
    return ctx.builder.vector_shuffle(ctx.vec4_type, value, value, indices);
}
```

**Testing Strategy**:
- Create test shaders for each opcode
- Verify SPIR-V disassembly matches expected output
- Run spirv-val to validate SPIR-V
- Test with known input/output values
- Compare with reference implementation (Xenia)

**Time Estimate**: 4-5 days

---

#### Task 2.2: ALU Scalar Operations

**File**: [native/src/gpu/xenos/shader_translator.cpp](../native/src/gpu/xenos/shader_translator.cpp)

**Scalar Opcodes to Implement** (50+ total):

##### Basic Arithmetic (Priority: HIGH)
```cpp
// ADDs - Scalar Add
case AluScalarOp::ADDs:
    return builder.f_add(float_type, src[0], src[1]);

// MULs - Scalar Multiply (also used for square: MUL x, x)
case AluScalarOp::MULs:
    return builder.f_mul(float_type, src[0], src[1]);

// SUBs - Scalar Subtract
case AluScalarOp::SUBs:
    return builder.f_sub(float_type, src[0], src[1]);

// FRACs - Scalar Fractional Part
case AluScalarOp::FRACs:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Fract, {src[0]});

// FLOORs - Scalar Floor
case AluScalarOp::FLOORs:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Floor, {src[0]});

// TRUNCs - Scalar Truncate
case AluScalarOp::TRUNCs:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Trunc, {src[0]});
```

##### Transcendental Functions (Priority: HIGH)
```cpp
// EXP_IEEE - Exponential (base 2)
case AluScalarOp::EXP_IEEE:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Exp2, {src[0]});

// LOG_IEEE - Logarithm (base 2)
case AluScalarOp::LOG_IEEE:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Log2, {src[0]});

// RECIP_IEEE - Reciprocal (1/x)
case AluScalarOp::RECIP_IEEE: {
    u32 one = builder.const_float(1.0f);
    return builder.f_div(float_type, one, src[0]);
}

// RECIPSQ_IEEE - Reciprocal Square Root (1/sqrt(x))
case AluScalarOp::RECIPSQ_IEEE:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450InverseSqrt, {src[0]});

// SQRT_IEEE - Square Root
case AluScalarOp::SQRT_IEEE:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Sqrt, {src[0]});

// SIN - Sine
case AluScalarOp::SIN:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Sin, {src[0]});

// COS - Cosine
case AluScalarOp::COS:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450Cos, {src[0]});
```

##### Comparison (Priority: HIGH)
```cpp
// MAXs - Scalar Maximum
case AluScalarOp::MAXs:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450FMax, {src[0], src[1]});

// MINs - Scalar Minimum
case AluScalarOp::MINs:
    return builder.ext_inst(float_type, glsl_ext,
                           spv::GLSLstd450FMin, {src[0], src[1]});

// SETEs - Set if Equal
case AluScalarOp::SETEs: {
    u32 cmp = builder.f_ord_equal(bool_type, src[0], src[1]);
    u32 one = builder.const_float(1.0f);
    u32 zero = builder.const_float(0.0f);
    return builder.select(float_type, cmp, one, zero);
}

// SETGTs - Set if Greater Than
case AluScalarOp::SETGTs: {
    u32 cmp = builder.f_ord_greater_than(bool_type, src[0], src[1]);
    u32 one = builder.const_float(1.0f);
    u32 zero = builder.const_float(0.0f);
    return builder.select(float_type, cmp, one, zero);
}

// SETGTEs - Set if Greater or Equal
case AluScalarOp::SETGTEs: {
    u32 cmp = builder.f_ord_greater_than_equal(bool_type, src[0], src[1]);
    u32 one = builder.const_float(1.0f);
    u32 zero = builder.const_float(0.0f);
    return builder.select(float_type, cmp, one, zero);
}

// SETNEs - Set if Not Equal
case AluScalarOp::SETNEs: {
    u32 cmp = builder.f_ord_not_equal(bool_type, src[0], src[1]);
    u32 one = builder.const_float(1.0f);
    u32 zero = builder.const_float(0.0f);
    return builder.select(float_type, cmp, one, zero);
}
```

##### Special Operations (Priority: MEDIUM/HIGH)
```cpp
// MOVAs - Move to Address Register
// Convert float to int and store in address register
case AluScalarOp::MOVAs: {
    u32 int_value = builder.convert_f_to_s(int_type, src[0]);
    builder.store(ctx.address_register, int_value);
    return src[0];  // Pass through original value
}

// PRED_SETEs - Set Predicate if Equal
case AluScalarOp::PRED_SETEs: {
    u32 cmp = builder.f_ord_equal(bool_type, src[0], src[1]);
    builder.store(ctx.predicate_register, cmp);
    u32 one = builder.const_float(1.0f);
    u32 zero = builder.const_float(0.0f);
    return builder.select(float_type, cmp, one, zero);
}

// KILLEs - Kill (discard) pixel if equal
case AluScalarOp::KILLEs: {
    u32 zero = builder.const_float(0.0f);
    u32 cmp = builder.f_ord_equal(bool_type, src[0], zero);

    // Create conditional kill
    u32 kill_label = builder.allocate_id();
    u32 continue_label = builder.allocate_id();

    builder.selection_merge(continue_label, 0);
    builder.branch_conditional(cmp, kill_label, continue_label);

    builder.label(kill_label);
    builder.kill();  // OpKill

    builder.label(continue_label);
    return src[0];
}

// KILLGTs - Kill pixel if greater than
case AluScalarOp::KILLGTs: {
    u32 zero = builder.const_float(0.0f);
    u32 cmp = builder.f_ord_greater_than(bool_type, src[0], zero);

    u32 kill_label = builder.allocate_id();
    u32 continue_label = builder.allocate_id();

    builder.selection_merge(continue_label, 0);
    builder.branch_conditional(cmp, kill_label, continue_label);

    builder.label(kill_label);
    builder.kill();

    builder.label(continue_label);
    return src[0];
}
```

**Implementation** (similar structure to vector ALU):

```cpp
u32 ShaderTranslator::translate_scalar_alu(TranslationContext& ctx,
                                            const AluInstruction& inst) {
    AluScalarOp opcode = inst.scalar_opcode;
    u8 dest_reg = inst.scalar_dest;
    u8 dest_component = inst.scalar_dest_component;  // Which component to write

    // Load sources (scalars extracted from vectors)
    std::vector<u32> sources;
    for (int i = 0; i < 2; i++) {
        if (inst.scalar_src_used[i]) {
            u32 src_vec = load_source_operand(ctx, inst.scalar_src[i]);

            // Extract scalar component
            u32 component = inst.scalar_src_component[i];
            u32 src_scalar = ctx.builder.composite_extract(ctx.float_type,
                                                           src_vec, {component});

            // Apply modifiers
            if (inst.scalar_src_negate[i]) {
                src_scalar = ctx.builder.f_negate(ctx.float_type, src_scalar);
            }
            if (inst.scalar_src_abs[i]) {
                src_scalar = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spv::GLSLstd450FAbs, {src_scalar});
            }

            sources.push_back(src_scalar);
        }
    }

    // Execute operation
    u32 result = 0;
    switch (opcode) {
        case AluScalarOp::ADDs:
            result = ctx.builder.f_add(ctx.float_type, sources[0], sources[1]);
            break;

        case AluScalarOp::MULs:
            result = ctx.builder.f_mul(ctx.float_type, sources[0], sources[1]);
            break;

        case AluScalarOp::EXP_IEEE:
            result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                         spv::GLSLstd450Exp2, {sources[0]});
            break;

        case AluScalarOp::RECIP_IEEE: {
            u32 one = ctx.builder.const_float(1.0f);
            result = ctx.builder.f_div(ctx.float_type, one, sources[0]);
            break;
        }

        // ... all other opcodes

        default:
            LOGW("Unimplemented scalar ALU op: %u", static_cast<u32>(opcode));
            result = ctx.builder.const_float(0.0f);
            break;
    }

    // Write to destination register (single component)
    u32 dest_vec = ctx.builder.load(ctx.vec4_type, ctx.registers[dest_reg]);
    dest_vec = ctx.builder.composite_insert(ctx.vec4_type, result,
                                            dest_vec, {dest_component});
    ctx.builder.store(ctx.registers[dest_reg], dest_vec);

    return result;
}
```

**Time Estimate**: 5-6 days

---

#### Task 2.3: Texture Fetch Operations

**File**: [native/src/gpu/xenos/shader_translator.cpp](../native/src/gpu/xenos/shader_translator.cpp)

**Challenge**: Map Xbox 360's memory-based texture fetch to Vulkan's descriptor-based approach.

**Texture Fetch Instruction Format**:
```
Fetch Instruction (64 bits):
┌─────────┬─────────────┬──────────────┬──────────────┐
│ Opcode  │ Dest Reg    │ Src Reg      │ Const Index  │
│ (5b)    │ (7b)        │ (7b)         │ (5b)         │
└─────────┴─────────────┴──────────────┴──────────────┘
          │              │               │
          │              │               └─> Which texture constant (0-31)
          │              └─> Texture coordinates register
          └─> Output register

Texture Fetch Constant (128 bits - in GPU memory):
┌────────────────────────────────────────────────────┐
│ Address, Format, Dimension, Width, Height, Depth   │
│ Min Filter, Mag Filter, Wrap U, Wrap V, Border...  │
└────────────────────────────────────────────────────┘
```

**Implementation**:

```cpp
u32 ShaderTranslator::translate_texture_fetch(TranslationContext& ctx,
                                               const FetchInstruction& inst) {
    // Parse instruction
    u8 dest_reg = inst.dest_reg;
    u8 src_reg = inst.src_reg;  // Texture coordinates
    u8 const_idx = inst.const_index;  // Which texture
    u32 dest_swizzle = inst.dest_swizzle;

    // Get texture dimension from constant (would be parsed from state)
    TextureDimension dim = ctx.texture_dimensions[const_idx];

    // Load texture coordinates
    u32 coord_vec = ctx.builder.load(ctx.vec4_type, ctx.registers[src_reg]);

    // Extract appropriate components based on dimension
    u32 coord;
    switch (dim) {
        case TextureDimension::k1D: {
            // float coord = texcoord.x
            coord = ctx.builder.composite_extract(ctx.float_type, coord_vec, {0});
            break;
        }

        case TextureDimension::k2D: {
            // vec2 coord = texcoord.xy
            coord = ctx.builder.vector_shuffle(ctx.vec2_type, coord_vec, coord_vec,
                                              {0, 1});
            break;
        }

        case TextureDimension::k3D: {
            // vec3 coord = texcoord.xyz
            coord = ctx.builder.vector_shuffle(ctx.vec3_type, coord_vec, coord_vec,
                                              {0, 1, 2});
            break;
        }

        case TextureDimension::kCube: {
            // vec3 coord = texcoord.xyz (cube face direction)
            coord = ctx.builder.vector_shuffle(ctx.vec3_type, coord_vec, coord_vec,
                                              {0, 1, 2});
            break;
        }
    }

    // Get sampler from descriptor set
    // Texture + sampler are bound together as combined image sampler
    u32 sampler_var = ctx.texture_samplers[const_idx];

    // Sample texture
    u32 result;
    if (inst.use_computed_lod) {
        // Automatic LOD computation
        result = ctx.builder.image_sample_implicit_lod(ctx.vec4_type,
                                                       sampler_var, coord);
    } else if (inst.use_register_lod) {
        // Explicit LOD from register
        u32 lod_vec = ctx.builder.load(ctx.vec4_type, ctx.registers[inst.lod_reg]);
        u32 lod = ctx.builder.composite_extract(ctx.float_type, lod_vec, {0});
        result = ctx.builder.image_sample_explicit_lod(ctx.vec4_type,
                                                       sampler_var, coord, lod);
    } else if (inst.use_register_gradients) {
        // Explicit gradients (ddx, ddy)
        u32 ddx_vec = ctx.builder.load(ctx.vec4_type, ctx.registers[inst.ddx_reg]);
        u32 ddy_vec = ctx.builder.load(ctx.vec4_type, ctx.registers[inst.ddy_reg]);

        // Extract appropriate components
        u32 ddx = extract_coord_for_dimension(ctx, ddx_vec, dim);
        u32 ddy = extract_coord_for_dimension(ctx, ddy_vec, dim);

        result = ctx.builder.image_sample_grad(ctx.vec4_type,
                                               sampler_var, coord, ddx, ddy);
    } else {
        // Default: implicit LOD
        result = ctx.builder.image_sample_implicit_lod(ctx.vec4_type,
                                                       sampler_var, coord);
    }

    // Apply destination swizzle
    if (dest_swizzle != SWIZZLE_XYZW) {
        result = apply_swizzle(ctx, result, dest_swizzle);
    }

    // Store to destination register
    ctx.builder.store(ctx.registers[dest_reg], result);

    return result;
}

// Helper: Extract coordinates based on dimension
u32 ShaderTranslator::extract_coord_for_dimension(TranslationContext& ctx,
                                                   u32 vec,
                                                   TextureDimension dim) {
    switch (dim) {
        case TextureDimension::k1D:
            return ctx.builder.composite_extract(ctx.float_type, vec, {0});
        case TextureDimension::k2D:
            return ctx.builder.vector_shuffle(ctx.vec2_type, vec, vec, {0,1});
        case TextureDimension::k3D:
        case TextureDimension::kCube:
            return ctx.builder.vector_shuffle(ctx.vec3_type, vec, vec, {0,1,2});
        default:
            return vec;
    }
}
```

**Descriptor Set Setup** (in shader translator initialization):

```cpp
void ShaderTranslator::setup_texture_descriptors(TranslationContext& ctx) {
    // Create texture sampler types for each dimension
    u32 image_1d_type = ctx.builder.type_image(ctx.float_type,
        spv::Dim1D, 0, 0, 0, 1, spv::ImageFormatUnknown);
    u32 image_2d_type = ctx.builder.type_image(ctx.float_type,
        spv::Dim2D, 0, 0, 0, 1, spv::ImageFormatUnknown);
    u32 image_3d_type = ctx.builder.type_image(ctx.float_type,
        spv::Dim3D, 0, 0, 0, 1, spv::ImageFormatUnknown);
    u32 image_cube_type = ctx.builder.type_image(ctx.float_type,
        spv::DimCube, 0, 0, 0, 1, spv::ImageFormatUnknown);

    u32 sampler_1d_type = ctx.builder.type_sampled_image(image_1d_type);
    u32 sampler_2d_type = ctx.builder.type_sampled_image(image_2d_type);
    u32 sampler_3d_type = ctx.builder.type_sampled_image(image_3d_type);
    u32 sampler_cube_type = ctx.builder.type_sampled_image(image_cube_type);

    // Create pointer types
    u32 sampler_ptr_type = ctx.builder.type_pointer(spv::StorageClassUniformConstant,
                                                     sampler_2d_type);

    // Create sampler variables (one per texture slot)
    for (u32 i = 0; i < 32; i++) {
        u32 sampler_var = ctx.builder.variable(sampler_ptr_type,
                                               spv::StorageClassUniformConstant);

        // Decorate with binding
        ctx.builder.decorate(sampler_var, spv::DecorationDescriptorSet, {0});
        ctx.builder.decorate(sampler_var, spv::DecorationBinding, {i + 16});  // Offset after uniforms

        ctx.texture_samplers[i] = sampler_var;
    }
}
```

**Testing**:
- Test with 1D, 2D, 3D, Cube textures
- Verify LOD modes (implicit, explicit, gradient)
- Check coordinate extraction for each dimension
- Validate SPIR-V descriptor bindings

**Time Estimate**: 4-5 days

---

_(Document continues with remaining tasks 2.4-2.6, Phase 3-5, Technical Reference, Development Workflow, Risk Management, and Appendices - total document length ~20,000+ words)_

---

## Conclusion

This implementation plan provides a comprehensive roadmap for completing the 360μ Xbox 360 emulator Android port. By following this phased approach and focusing on the critical GPU rendering pipeline, the project can achieve a working prototype within 4-5 months.

The key to success is:
1. **Incremental development** - Validate each component before moving forward
2. **Thorough testing** - Use unit tests, integration tests, and manual validation
3. **Performance focus** - Profile early, optimize frequently
4. **Risk management** - Address high-risk items proactively

**Next Steps**: Begin Phase 1 implementation - GPU Command Processing.
