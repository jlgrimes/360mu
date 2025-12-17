# 360μ - Xbox 360 Emulator for Android

## Comprehensive Development Plan

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Xbox 360 Hardware Architecture](#2-xbox-360-hardware-architecture)
3. [Emulator Architecture Design](#3-emulator-architecture-design)
4. [Core Components](#4-core-components)
5. [Android-Specific Considerations](#5-android-specific-considerations)
6. [Performance Optimization Strategies](#6-performance-optimization-strategies)
7. [Development Phases](#7-development-phases)
8. [Testing Strategy](#8-testing-strategy)
9. [Tools & Dependencies](#9-tools--dependencies)
10. [Risk Assessment](#10-risk-assessment)

---

## 1. Project Overview

### 1.1 Vision

Create a high-performance Xbox 360 emulator for Android that achieves playable framerates on flagship mobile devices through aggressive optimization and modern emulation techniques.

### 1.2 Target Specifications

- **Minimum Android Version:** Android 10 (API 29)
- **Target Devices:** Snapdragon 8 Gen 2+, Dimensity 9000+, or equivalent
- **GPU API:** Vulkan 1.1+ (primary), OpenGL ES 3.2 (fallback)
- **RAM Requirement:** 8GB+ recommended
- **Target Performance:** 30fps for lighter titles, 60fps stretch goal

### 1.3 Technology Stack

- **Language:** C++ (core), Kotlin (Android UI), Assembly (critical paths)
- **Build System:** CMake + Gradle
- **Graphics:** Vulkan with SPIR-V shaders
- **Audio:** AAudio/OpenSL ES
- **JIT Framework:** Custom dynarec or AArch64 backend

---

## 2. Xbox 360 Hardware Architecture

### 2.1 CPU: IBM Xenon

```
┌─────────────────────────────────────────────────────────────┐
│                     IBM XENON CPU                           │
├─────────────────────────────────────────────────────────────┤
│  Architecture: PowerPC 64-bit (modified)                    │
│  Cores: 3 symmetric cores                                   │
│  Threads: 6 (2 hardware threads per core via SMT)           │
│  Clock Speed: 3.2 GHz                                       │
│  L1 Cache: 32KB I-cache + 32KB D-cache per core             │
│  L2 Cache: 1MB shared                                       │
│  Vector Unit: VMX128 (128-bit SIMD, Xbox-specific ext)      │
│  Byte Order: Big-endian                                     │
└─────────────────────────────────────────────────────────────┘
```

**Key CPU Features to Emulate:**

- PowerPC instruction set with Xbox extensions
- VMX128 vector instructions (similar to AltiVec but extended)
- Hardware threading (SMT)
- Memory protection and page tables
- Performance counters
- Privileged mode operations

### 2.2 GPU: ATI Xenos

```
┌─────────────────────────────────────────────────────────────┐
│                     ATI XENOS GPU                           │
├─────────────────────────────────────────────────────────────┤
│  Architecture: Unified shader architecture                  │
│  Shader Processors: 48 unified shaders                      │
│  Clock Speed: 500 MHz                                       │
│  Memory: 512MB GDDR3 (shared with CPU)                      │
│  Memory Bandwidth: 22.4 GB/s                                │
│  eDRAM: 10MB (for render targets, with special resolve)     │
│  Shader Model: 3.0+ with Xbox extensions                    │
│  Texture Units: 16                                          │
│  ROPs: 8 (but eDRAM enables much higher effective)          │
└─────────────────────────────────────────────────────────────┘
```

**Critical GPU Features:**

- **Unified Shader Architecture:** First consumer GPU with this - shaders can be vertex/pixel
- **10MB eDRAM:** Ultra-fast embedded memory for render targets
- **Tiled Rendering:** Hardware predicated tiling for the eDRAM
- **Resolve Operations:** Special hardware for eDRAM→main memory transfers
- **Shader Microcode:** Custom format, not standard DirectX bytecode
- **Texture Formats:** Xbox-specific compressed formats
- **GPU Command Buffer:** Ring buffer with specialized packets

### 2.3 Memory Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   UNIFIED MEMORY MAP                        │
├─────────────────────────────────────────────────────────────┤
│  0x00000000 - 0x1FFFFFFF: Main RAM (512MB)                  │
│  0x20000000 - 0x3FFFFFFF: Secondary RAM region              │
│  0x7FE00000 - 0x7FFFFFFF: GPU Registers                     │
│  0x80000000 - 0x8FFFFFFF: Physical mapping                  │
│  0xC0000000 - 0xFFFFFFFF: GPU Command Buffers               │
│                                                             │
│  eDRAM: Separate 10MB, accessed via GPU only                │
└─────────────────────────────────────────────────────────────┘
```

### 2.4 Audio: XMA

- Custom codec based on WMA Pro
- Hardware decoder on the southbridge
- 256 audio channels
- 48kHz sample rate
- XMA2 format support

### 2.5 I/O Systems

- SATA for HDD/DVD
- USB 2.0 for controllers
- Ethernet
- Wireless (802.11a/b/g)

---

## 3. Emulator Architecture Design

### 3.1 High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                        ANDROID APPLICATION                         │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                      Kotlin UI Layer                          │  │
│  │   • Game Browser  • Settings  • Controller Config  • Overlay │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                              │ JNI                                 │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    Native Core (C++)                          │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐   │  │
│  │  │ CPU Module  │  │ GPU Module  │  │   System Module     │   │  │
│  │  │             │  │             │  │                     │   │  │
│  │  │ • Xenon     │  │ • Xenos     │  │ • Memory Manager    │   │  │
│  │  │ • JIT/AOT   │  │ • Vulkan    │  │ • Kernel Services   │   │  │
│  │  │ • VMX128    │  │ • Shaders   │  │ • File System       │   │  │
│  │  │ • Threading │  │ • eDRAM     │  │ • XAM/XEX Loader    │   │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────────┘   │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐   │  │
│  │  │Audio Module │  │Input Module │  │   Network Module    │   │  │
│  │  │             │  │             │  │                     │   │  │
│  │  │ • XMA Decode│  │ • Gamepad   │  │ • Xbox Live Stub    │   │  │
│  │  │ • AAudio    │  │ • Keyboard  │  │ • System Link       │   │  │
│  │  │ • Mixing    │  │ • Touch     │  │ • HTTP Client       │   │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 Module Communication

```
┌─────────────────────────────────────────────────────────────────┐
│                    EVENT BUS / MESSAGE QUEUE                     │
├─────────────────────────────────────────────────────────────────┤
│  • Lock-free SPSC queues for inter-module communication          │
│  • GPU command submission via ring buffer                        │
│  • Audio mixing via shared circular buffers                      │
│  • Input events via priority queue                               │
└─────────────────────────────────────────────────────────────────┘
```

### 3.3 Threading Model

```
┌─────────────────────────────────────────────────────────────────┐
│                      THREAD ARCHITECTURE                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────┐   ┌──────────────────┐                    │
│  │  CPU Thread 0    │   │  CPU Thread 1    │  (Xenon Core 0)    │
│  └──────────────────┘   └──────────────────┘                    │
│  ┌──────────────────┐   ┌──────────────────┐                    │
│  │  CPU Thread 2    │   │  CPU Thread 3    │  (Xenon Core 1)    │
│  └──────────────────┘   └──────────────────┘                    │
│  ┌──────────────────┐   ┌──────────────────┐                    │
│  │  CPU Thread 4    │   │  CPU Thread 5    │  (Xenon Core 2)    │
│  └──────────────────┘   └──────────────────┘                    │
│                                                                  │
│  ┌──────────────────┐   ┌──────────────────┐                    │
│  │   GPU Thread     │   │  Audio Thread    │                    │
│  └──────────────────┘   └──────────────────┘                    │
│                                                                  │
│  ┌──────────────────┐   ┌──────────────────┐                    │
│  │   I/O Thread     │   │  Network Thread  │                    │
│  └──────────────────┘   └──────────────────┘                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

Thread Affinity Strategy:
- Pin CPU emulation threads to big cores
- GPU thread on big core with highest priority
- Audio thread on medium core with real-time priority
- I/O and network on little cores
```

---

## 4. Core Components

### 4.1 CPU Emulation (PowerPC Xenon)

#### 4.1.1 Recompilation Strategy

```
┌─────────────────────────────────────────────────────────────────┐
│                    JIT COMPILATION PIPELINE                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐  │
│  │  PPC     │───▶│  IR      │───▶│ Optimize │───▶│  ARM64   │  │
│  │  Decode  │    │  Build   │    │  Pass    │    │  Emit    │  │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘  │
│                                                                  │
│  Features:                                                       │
│  • Block-based compilation with basic block detection            │
│  • Hot path optimization (profile-guided)                        │
│  • Lazy compilation for infrequently executed code               │
│  • Code cache management (LRU eviction)                          │
│  • Self-modifying code detection                                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.1.2 Register Mapping

```cpp
// PPC to ARM64 Register Allocation Strategy
//
// ARM64 has 31 general-purpose registers (X0-X30)
// PPC has 32 GPRs (r0-r31) + 32 FPRs + 128 VMX registers
//
// Static allocation for hot registers:
//   X19-X28: PPC r0-r9 (callee-saved, always available)
//   X9-X18:  PPC r10-r19 (caller-saved, spill on calls)
//
// FPU: Use ARM64 NEON V0-V31 for both FPR and VMX
//   V0-V15:  Scratch / arguments
//   V16-V31: PPC FPR0-FPR15 (callee-saved subset)
//
// VMX128 registers require memory backing due to count
```

#### 4.1.3 VMX128 Emulation

```
┌─────────────────────────────────────────────────────────────────┐
│                  VMX128 SIMD TRANSLATION                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Xbox VMX128 is a superset of AltiVec with:                      │
│  • 128 vector registers (vs 32 in standard AltiVec)              │
│  • Additional dot product instructions                           │
│  • Specialized permute operations                                │
│                                                                  │
│  Translation to ARM64 NEON:                                      │
│  ┌────────────────┐         ┌────────────────┐                  │
│  │  vaddfp        │   ───▶  │  fadd Vd.4S    │                  │
│  │  vmulfp        │   ───▶  │  fmul Vd.4S    │                  │
│  │  vmaddfp       │   ───▶  │  fmla Vd.4S    │                  │
│  │  vperm         │   ───▶  │  tbl + tbx     │  (complex)       │
│  │  vdot128       │   ───▶  │  fmul + faddp  │  (multi-inst)    │
│  └────────────────┘         └────────────────┘                  │
│                                                                  │
│  Strategy: Register windowing with 32-register working set       │
│  Spill to memory for full 128-register support                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.1.4 Memory Access Handling

```cpp
// Fastmem implementation using page fault handling
//
// 1. Reserve large virtual address range (4GB+)
// 2. Map guest memory pages on-demand
// 3. Install SIGSEGV handler for unmapped accesses
// 4. Handler checks if valid Xbox memory region:
//    - If valid: map page and resume
//    - If MMIO: dispatch to device emulation
//    - If invalid: raise guest exception

struct FastmemRegion {
    void* host_base;        // mmap'd region
    uint64_t guest_base;    // Xbox virtual address start
    uint64_t size;          // Region size
    uint32_t permissions;   // R/W/X
};

// MMIO regions require special handling:
// - GPU registers: 0x7FE00000
// - Audio hardware: 0x7FEA0000
// - Crypto unit: 0x7FE80000
```

### 4.2 GPU Emulation (ATI Xenos)

#### 4.2.1 Xenos Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    XENOS GPU PIPELINE                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐│
│  │Command │──▶│Geometry│──▶│Unified │──▶│Texture │──▶│ Output ││
│  │Process │   │ Fetch  │   │Shaders │   │ Units  │   │ Merger ││
│  └────────┘   └────────┘   └────────┘   └────────┘   └────────┘│
│      │                          │                         │      │
│      │                          │                         ▼      │
│      │                          │                    ┌────────┐  │
│      │                          │                    │ eDRAM  │  │
│      │                          │                    │ (10MB) │  │
│      │                          │                    └────────┘  │
│      │                          │                         │      │
│      │                          │                         ▼      │
│      │                          │                    ┌────────┐  │
│      └──────────────────────────┴───────────────────▶│Resolve │  │
│                                                      │  Unit  │  │
│                                                      └────────┘  │
│                                                           │      │
│                                                           ▼      │
│                                                     Main Memory  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.2.2 Shader Translation

```
┌─────────────────────────────────────────────────────────────────┐
│                 SHADER TRANSLATION PIPELINE                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────┐                                               │
│  │ Xenos Shader  │  (Raw microcode from game)                    │
│  │  Microcode    │                                               │
│  └───────┬───────┘                                               │
│          │                                                       │
│          ▼                                                       │
│  ┌───────────────┐                                               │
│  │   Disassemble │  Parse Xenos instruction format               │
│  │   & Analyze   │  Build control flow graph                     │
│  └───────┬───────┘                                               │
│          │                                                       │
│          ▼                                                       │
│  ┌───────────────┐                                               │
│  │  IR Builder   │  Convert to internal representation          │
│  │               │  Handle Xbox-specific features                │
│  └───────┬───────┘                                               │
│          │                                                       │
│          ▼                                                       │
│  ┌───────────────┐                                               │
│  │  Optimization │  Dead code elimination                       │
│  │     Pass      │  Register allocation                         │
│  └───────┬───────┘                                               │
│          │                                                       │
│          ▼                                                       │
│  ┌───────────────┐                                               │
│  │  SPIR-V Gen   │  Emit Vulkan-compatible SPIR-V               │
│  │               │  Handle precision differences                 │
│  └───────┬───────┘                                               │
│          │                                                       │
│          ▼                                                       │
│  ┌───────────────┐                                               │
│  │ Shader Cache  │  Hash-based caching of compiled shaders      │
│  │               │  Persistent storage on device                 │
│  └───────────────┘                                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.2.3 eDRAM Emulation Strategy

```
┌─────────────────────────────────────────────────────────────────┐
│                    eDRAM EMULATION                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  The 10MB eDRAM is the most challenging GPU feature:             │
│  • Games render to eDRAM at native resolution                    │
│  • "Resolve" operation copies to main memory                     │
│  • Hardware supports "predicated tiling" for >10MB targets       │
│  • Free 4x MSAA via eDRAM bandwidth                              │
│                                                                  │
│  Emulation Approaches:                                           │
│                                                                  │
│  Option A: Tile-Based Rendering (Recommended for Mobile)         │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Use Vulkan subpasses to emulate eDRAM behavior            │ │
│  │ • Leverage mobile GPU tile memory as virtual eDRAM          │ │
│  │ • VK_EXT_load_store_op_none for efficient clears            │ │
│  │ • Multi-pass rendering for complex resolve operations       │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Option B: Image Memory (Fallback)                               │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Allocate VkImage for eDRAM simulation                     │ │
│  │ • Use compute shaders for resolve operations                │ │
│  │ • Higher memory bandwidth requirements                      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Resolve Operations to Handle:                                   │
│  • Copy: eDRAM → Main memory (basic blit)                       │
│  • Clear: Fast fill operations                                   │
│  • Downsample: MSAA resolve                                      │
│  • Format conversion: Various surface formats                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.2.4 Texture Handling

```
┌─────────────────────────────────────────────────────────────────┐
│                   TEXTURE FORMATS                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Xbox 360 Texture Formats → Vulkan Equivalents:                  │
│                                                                  │
│  ┌────────────────────┬─────────────────────────────────────┐   │
│  │ Xbox Format        │ Vulkan Format                       │   │
│  ├────────────────────┼─────────────────────────────────────┤   │
│  │ DXT1 (k_8)         │ VK_FORMAT_BC1_RGBA_UNORM_BLOCK      │   │
│  │ DXT2/3 (k_3)       │ VK_FORMAT_BC2_UNORM_BLOCK           │   │
│  │ DXT4/5 (k_5)       │ VK_FORMAT_BC3_UNORM_BLOCK           │   │
│  │ CTX1 (k_11)        │ Custom decode → R8G8                │   │
│  │ DXN (k_12)         │ VK_FORMAT_BC5_UNORM_BLOCK           │   │
│  │ A8R8G8B8           │ VK_FORMAT_B8G8R8A8_UNORM (swizzle)  │   │
│  │ LIN_A8R8G8B8       │ VK_FORMAT_B8G8R8A8_UNORM            │   │
│  │ 16F/32F            │ VK_FORMAT_R16_SFLOAT etc.           │   │
│  └────────────────────┴─────────────────────────────────────┘   │
│                                                                  │
│  Texture Tiling:                                                 │
│  • Xbox uses Morton/Z-order tiling for textures                  │
│  • Must detile on load (compute shader or CPU)                   │
│  • Cache detiled textures for reuse                              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.3 Audio Emulation

#### 4.3.1 XMA Decoder

```
┌─────────────────────────────────────────────────────────────────┐
│                    XMA AUDIO PIPELINE                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │  XMA Stream  │───▶│  XMA Decode  │───▶│  PCM Output  │       │
│  │  (from game) │    │  (software)  │    │  (16-bit)    │       │
│  └──────────────┘    └──────────────┘    └──────────────┘       │
│                                                                  │
│  XMA Format Details:                                             │
│  • Based on WMA Pro codec                                        │
│  • Packet-based format with seek tables                          │
│  • Supports mono/stereo/5.1                                      │
│  • Hardware provides context switching for 256 voices            │
│                                                                  │
│  Implementation Options:                                         │
│                                                                  │
│  1. FFmpeg Integration (Recommended)                             │
│     • FFmpeg has XMA decoder (libavcodec)                        │
│     • Wrap in threaded decoder                                   │
│     • Buffer management for streaming                            │
│                                                                  │
│  2. Custom Decoder                                               │
│     • Port from Xenia's implementation                           │
│     • SIMD-optimized for NEON                                    │
│     • More control but significant work                          │
│                                                                  │
│  Audio Output:                                                   │
│  • AAudio (Android 8.1+) - low latency                          │
│  • OpenSL ES - fallback                                          │
│  • Target: <20ms latency                                         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.4 Xbox Kernel Emulation

#### 4.4.1 Kernel Services (HLE)

```
┌─────────────────────────────────────────────────────────────────┐
│              HIGH-LEVEL EMULATION (HLE) SERVICES                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  XEX Loader:                                                     │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Parse XEX2 format headers                                 │ │
│  │ • Decompress/decrypt executable                             │ │
│  │ • Handle imports (resolve to HLE functions)                 │ │
│  │ • Setup initial thread context                              │ │
│  │ • Load required system modules (xam.xex, etc.)              │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Threading APIs (KeXxx):                                         │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • KeInitializeThread                                        │ │
│  │ • KeWaitForSingleObject                                     │ │
│  │ • KeSetEvent / KeResetEvent                                 │ │
│  │ • KeSynchronizeExecution                                    │ │
│  │ • KeAcquireSpinLock / KeReleaseSpinLock                     │ │
│  │                                                             │ │
│  │ Implementation: Map to pthreads with priority handling      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Memory APIs (MmXxx, NtXxx):                                     │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • NtAllocateVirtualMemory                                   │ │
│  │ • NtFreeVirtualMemory                                       │ │
│  │ • MmGetPhysicalAddress                                      │ │
│  │ • MmMapIoSpace                                              │ │
│  │                                                             │ │
│  │ Implementation: Custom allocator with Xbox memory map       │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  File System APIs (NtXxx):                                       │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • NtCreateFile / NtOpenFile                                 │ │
│  │ • NtReadFile / NtWriteFile                                  │ │
│  │ • NtQueryInformationFile                                    │ │
│  │ • NtQueryDirectoryFile                                      │ │
│  │                                                             │ │
│  │ Paths to handle:                                            │ │
│  │   \Device\Harddisk0\Partition1\ → Game data                 │ │
│  │   \Device\Cdrom0\ → ISO/XGD mount                          │ │
│  │   \Device\Flash\ → NAND emulation                          │ │
│  │   game:\ → Current title root                              │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  XAM (Xbox Application Manager):                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • XamUserGetSigninState                                     │ │
│  │ • XamShowMessageBoxUI                                       │ │
│  │ • XamContentCreate / XamContentClose                        │ │
│  │ • Achievement APIs                                          │ │
│  │ • Profile/Save data management                              │ │
│  │                                                             │ │
│  │ Implementation: Stub most, implement save/load critical     │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.4.2 Import Resolution Table

```cpp
// Example import table structure
struct KernelImport {
    const char* module;      // e.g., "xboxkrnl.exe"
    uint32_t ordinal;        // Import ordinal
    const char* name;        // Optional name
    void* hle_handler;       // Our implementation
};

// Critical imports (~200 commonly used)
static KernelImport imports[] = {
    {"xboxkrnl.exe", 1,   "NtAllocateVirtualMemory", &HLE_NtAllocateVirtualMemory},
    {"xboxkrnl.exe", 3,   "NtClose", &HLE_NtClose},
    {"xboxkrnl.exe", 66,  "KeWaitForSingleObject", &HLE_KeWaitForSingleObject},
    {"xboxkrnl.exe", 190, "RtlEnterCriticalSection", &HLE_RtlEnterCriticalSection},
    {"xam.xex",      5,   "XamUserGetSigninState", &HLE_XamUserGetSigninState},
    // ... hundreds more
};
```

---

## 5. Android-Specific Considerations

### 5.1 Performance on Mobile SoCs

```
┌─────────────────────────────────────────────────────────────────┐
│            MOBILE SoC CONSIDERATIONS                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Snapdragon 8 Gen 3 Example:                                     │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ CPU: 1x Cortex-X4 @ 3.3GHz (Prime)                          │ │
│  │      3x Cortex-A720 @ 3.2GHz (Performance)                  │ │
│  │      4x Cortex-A520 @ 2.3GHz (Efficiency)                   │ │
│  │ GPU: Adreno 750 (~2.5 TFLOPS)                               │ │
│  │ RAM: LPDDR5X @ 4200MHz                                      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Thermal Constraints:                                            │
│  • Sustained performance ~60-70% of peak                         │
│  • Throttling begins after 10-15 minutes                         │
│  • Must implement adaptive quality/performance                   │
│                                                                  │
│  Strategies:                                                     │
│  1. Dynamic Resolution Scaling (DRS)                             │
│     - Lower internal resolution when GPU-bound                   │
│     - Use FSR/resolution reconstruction                          │
│                                                                  │
│  2. Frame Pacing                                                 │
│     - Target consistent 30fps over variable                      │
│     - Use VSync with frame doubling                              │
│                                                                  │
│  3. Thread Migration                                             │
│     - Move compute to little cores when thermal budget low       │
│     - Batch I/O operations during idle periods                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Vulkan on Android

```
┌─────────────────────────────────────────────────────────────────┐
│               VULKAN ANDROID SPECIFICS                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Required Extensions:                                            │
│  • VK_KHR_swapchain                                              │
│  • VK_KHR_maintenance1/2/3                                       │
│  • VK_EXT_descriptor_indexing (for bindless textures)            │
│  • VK_KHR_push_descriptor (faster uniform updates)               │
│                                                                  │
│  Mobile-Optimized Features:                                      │
│  • VK_EXT_subgroup_size_control (tune for GPU wave size)         │
│  • VK_EXT_memory_budget (manage limited VRAM)                    │
│  • VK_QCOM_render_pass_transform (free rotation)                 │
│  • VK_EXT_fragment_shading_rate (VRS for performance)            │
│                                                                  │
│  Tile-Based GPU Optimization:                                    │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Use VK_ATTACHMENT_LOAD_OP_DONT_CARE when possible         │ │
│  │ • Keep render passes short and efficient                    │ │
│  │ • Avoid unnecessary STORE operations                        │ │
│  │ • Use transient attachments for depth/stencil               │ │
│  │ • Leverage subpass dependencies for on-chip resolution      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Memory Management:                                              │
│  • Use VMA (Vulkan Memory Allocator)                             │
│  • Pool allocations for similar-sized resources                  │
│  • Defragment periodically during loading screens                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.3 JNI Bridge Design

```cpp
// Native interface exposed to Kotlin
extern "C" {
    // Lifecycle
    JNIEXPORT jlong JNICALL Java_com_x360mu_core_Emulator_init(JNIEnv* env, jobject obj);
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_destroy(JNIEnv* env, jobject obj, jlong handle);

    // Game loading
    JNIEXPORT jboolean JNICALL Java_com_x360mu_core_Emulator_loadGame(
        JNIEnv* env, jobject obj, jlong handle, jstring path);

    // Execution control
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_run(JNIEnv* env, jobject obj, jlong handle);
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_pause(JNIEnv* env, jobject obj, jlong handle);
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_stop(JNIEnv* env, jobject obj, jlong handle);

    // Graphics
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_setSurface(
        JNIEnv* env, jobject obj, jlong handle, jobject surface);

    // Input
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_onGamepadEvent(
        JNIEnv* env, jobject obj, jlong handle, jint player, jint button, jfloat value);

    // Settings
    JNIEXPORT void JNICALL Java_com_x360mu_core_Emulator_setOption(
        JNIEnv* env, jobject obj, jlong handle, jstring key, jstring value);

    // Save states
    JNIEXPORT jboolean JNICALL Java_com_x360mu_core_Emulator_saveState(
        JNIEnv* env, jobject obj, jlong handle, jstring path);
    JNIEXPORT jboolean JNICALL Java_com_x360mu_core_Emulator_loadState(
        JNIEnv* env, jobject obj, jlong handle, jstring path);
}
```

### 5.4 Android UI Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                   ANDROID UI STRUCTURE                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Technology: Jetpack Compose (Modern, declarative UI)            │
│                                                                  │
│  Screen Flow:                                                    │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐     │
│  │  Splash  │──▶│  Library │──▶│ Settings │──▶│   Game   │     │
│  │  Screen  │   │  Browser │   │  Screen  │   │  Screen  │     │
│  └──────────┘   └──────────┘   └──────────┘   └──────────┘     │
│                       │              │                           │
│                       ▼              ▼                           │
│                 ┌──────────┐   ┌──────────┐                     │
│                 │  Search  │   │ Per-Game │                     │
│                 │  Filter  │   │ Settings │                     │
│                 └──────────┘   └──────────┘                     │
│                                                                  │
│  Game Screen Components:                                         │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                    SurfaceView                              │ │
│  │                   (Vulkan render)                           │ │
│  │                                                             │ │
│  │  ┌───────────────────────────────────────────────────────┐  │ │
│  │  │              Touch Controller Overlay                  │  │ │
│  │  │   ┌─────┐                           ┌─────┐           │  │ │
│  │  │   │D-Pad│    [START] [BACK]         │ABXY │           │  │ │
│  │  │   └─────┘                           └─────┘           │  │ │
│  │  │   ┌─────┐                           ┌─────┐           │  │ │
│  │  │   │ L   │    [LB] [LT] [RT] [RB]    │  R  │           │  │ │
│  │  │   │Stick│                           │Stick│           │  │ │
│  │  │   └─────┘                           └─────┘           │  │ │
│  │  └───────────────────────────────────────────────────────┘  │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. Performance Optimization Strategies

### 6.1 CPU Optimization

#### 6.1.1 JIT Compiler Optimizations

```
┌─────────────────────────────────────────────────────────────────┐
│                  JIT OPTIMIZATION PASSES                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Pass 1: Basic Block Analysis                                    │
│  • Identify loop headers and hot paths                           │
│  • Mark frequently executed blocks for optimization              │
│  • Detect self-modifying code regions                            │
│                                                                  │
│  Pass 2: Constant Propagation                                    │
│  • Track known constant values through blocks                    │
│  • Fold arithmetic with known operands                           │
│  • Eliminate dead comparisons                                    │
│                                                                  │
│  Pass 3: Register Allocation                                     │
│  • Linear scan allocator for speed                               │
│  • Prioritize loop-carried values                                │
│  • Minimize spills in hot loops                                  │
│                                                                  │
│  Pass 4: Instruction Selection                                   │
│  • Pattern match PPC → ARM64 idioms                              │
│  • Use ARM64-specific instructions (MADD, etc.)                  │
│  • Combine load/store pairs                                      │
│                                                                  │
│  Pass 5: Peephole Optimization                                   │
│  • Remove redundant moves                                        │
│  • Strength reduction                                            │
│  • Branch straightening                                          │
│                                                                  │
│  Execution Tiers:                                                │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ Tier 0: Interpreter (cold code, fast startup)               │ │
│  │ Tier 1: Baseline JIT (moderate optimization)                │ │
│  │ Tier 2: Optimizing JIT (hot code, full optimization)        │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 6.1.2 SIMD Optimization for VMX128

```cpp
// Example: Optimized dot product (common in games)
// PPC VMX128:
//   vdot128 v0, v1, v2    ; v0 = dot(v1, v2) broadcast to all lanes

// ARM64 NEON equivalent:
//   fmul v3.4s, v1.4s, v2.4s    ; component-wise multiply
//   faddp v3.4s, v3.4s, v3.4s   ; pairwise add
//   faddp v0.4s, v3.4s, v3.4s   ; final sum
//   dup v0.4s, v0.s[0]          ; broadcast to all lanes

// Intrinsic implementation
inline float32x4_t vdot128_emulate(float32x4_t a, float32x4_t b) {
    float32x4_t prod = vmulq_f32(a, b);
    float32x2_t sum1 = vpadd_f32(vget_low_f32(prod), vget_high_f32(prod));
    float32x2_t sum2 = vpadd_f32(sum1, sum1);
    return vdupq_lane_f32(sum2, 0);
}
```

### 6.2 GPU Optimization

#### 6.2.1 Shader Compilation Strategy

```
┌─────────────────────────────────────────────────────────────────┐
│               SHADER CACHING STRATEGY                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Multi-Level Cache:                                              │
│                                                                  │
│  Level 1: In-Memory LRU Cache                                    │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • VkPipeline objects ready for immediate use                │ │
│  │ • Limited by available memory (~256-512 entries)            │ │
│  │ • Eviction based on LRU + access frequency                  │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Level 2: Pipeline Cache (Vulkan native)                         │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • VkPipelineCache persisted to disk                         │ │
│  │ • Faster recreation than full compile                       │ │
│  │ • Device-specific, invalidated on driver update             │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Level 3: SPIR-V Cache                                           │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Pre-translated SPIR-V blobs                               │ │
│  │ • Keyed by Xenos shader hash                                │ │
│  │ • Device-agnostic, survives driver updates                  │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Background Compilation:                                         │
│  • Compile shaders on separate thread                            │
│  • Use placeholder/fallback while compiling                      │
│  • Precompile known shaders during game load                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 6.2.2 Draw Call Batching

```
┌─────────────────────────────────────────────────────────────────┐
│                DRAW CALL OPTIMIZATION                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Xbox 360 games can issue thousands of draw calls per frame.     │
│  Mobile GPUs prefer fewer, larger draws.                         │
│                                                                  │
│  Strategies:                                                     │
│                                                                  │
│  1. Instanced Drawing                                            │
│     • Detect repeated draws with same state                      │
│     • Convert to instanced draw calls                            │
│     • Significant for particle systems, foliage                  │
│                                                                  │
│  2. Indirect Drawing                                             │
│     • Batch similar draws into indirect buffer                   │
│     • Single vkCmdDrawIndirect for many objects                  │
│     • GPU-driven rendering where possible                        │
│                                                                  │
│  3. State Sorting                                                │
│     • Sort draws by pipeline/descriptor state                    │
│     • Minimize state changes between draws                       │
│     • May diverge from original draw order                       │
│     • Only for opaque geometry                                   │
│                                                                  │
│  4. Descriptor Management                                        │
│     • Use VK_EXT_descriptor_indexing for bindless                │
│     • Single descriptor set for all textures                     │
│     • Dynamic uniform buffers for per-draw data                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 6.3 Memory Optimization

```
┌─────────────────────────────────────────────────────────────────┐
│               MEMORY MANAGEMENT                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Guest Memory Layout (512MB emulated):                           │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Main allocation: mmap'd region with fastmem support       │ │
│  │ • Guard pages for out-of-bounds detection                   │ │
│  │ • Write-tracking for GPU texture invalidation               │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Host Memory Budget (8GB device):                                │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ Guest RAM:        512 MB                                    │ │
│  │ JIT Code Cache:   128 MB                                    │ │
│  │ Shader Cache:     64 MB                                     │ │
│  │ Texture Cache:    256 MB (with streaming)                   │ │
│  │ Vulkan Buffers:   128 MB                                    │ │
│  │ Audio Buffers:    16 MB                                     │ │
│  │ ──────────────────────────                                  │ │
│  │ Total Target:     ~1.1 GB (leaves room for OS/apps)         │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Texture Streaming:                                              │
│  • Load textures on-demand at used mip level                     │
│  • Background loading of upcoming textures                       │
│  • Evict unused textures under memory pressure                   │
│  • Quality reduction on low-memory devices                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. Development Phases

### Phase 1: Foundation (Months 1-4)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PHASE 1: FOUNDATION                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Milestone 1.1: Project Setup (2 weeks)                          │
│  ☐ Android Studio project with Gradle                            │
│  ☐ CMake build system for native code                            │
│  ☐ CI/CD pipeline (GitHub Actions)                               │
│  ☐ Code style and contribution guidelines                        │
│  ☐ Initial Kotlin UI scaffolding                                 │
│                                                                  │
│  Milestone 1.2: Memory System (3 weeks)                          │
│  ☐ Guest memory allocator                                        │
│  ☐ Fastmem implementation with SIGSEGV handler                   │
│  ☐ Memory-mapped I/O framework                                   │
│  ☐ Page table emulation                                          │
│                                                                  │
│  Milestone 1.3: XEX Loader (4 weeks)                             │
│  ☐ XEX2 format parser                                            │
│  ☐ Executable decryption (for homebrew/dev-signed)               │
│  ☐ Import table resolution                                       │
│  ☐ Initial HLE kernel stubs                                      │
│  ☐ Basic file system virtualization                              │
│                                                                  │
│  Milestone 1.4: CPU Interpreter (4 weeks)                        │
│  ☐ PowerPC instruction decoder                                   │
│  ☐ Full integer instruction set                                  │
│  ☐ FPU instructions (basic)                                      │
│  ☐ Condition register handling                                   │
│  ☐ System call dispatch                                          │
│                                                                  │
│  Deliverable: Boot simple homebrew to main()                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Phase 2: CPU JIT (Months 5-8)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PHASE 2: CPU JIT                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Milestone 2.1: JIT Framework (4 weeks)                          │
│  ☐ IR design and implementation                                  │
│  ☐ ARM64 code emitter                                            │
│  ☐ Block-based compilation                                       │
│  ☐ Code cache management                                         │
│                                                                  │
│  Milestone 2.2: Full PPC Support (6 weeks)                       │
│  ☐ All integer instructions JIT'd                                │
│  ☐ All FPU instructions JIT'd                                    │
│  ☐ Branch and link handling                                      │
│  ☐ Exception handling framework                                  │
│                                                                  │
│  Milestone 2.3: VMX128 Emulation (4 weeks)                       │
│  ☐ Standard AltiVec instructions                                 │
│  ☐ Xbox-specific VMX128 extensions                               │
│  ☐ 128-register windowing system                                 │
│  ☐ NEON optimization for common patterns                         │
│                                                                  │
│  Milestone 2.4: Threading (2 weeks)                              │
│  ☐ Multi-threaded CPU emulation                                  │
│  ☐ Thread synchronization primitives                             │
│  ☐ Per-thread JIT contexts                                       │
│                                                                  │
│  Deliverable: Run CPU-bound homebrew at playable speeds          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Phase 3: GPU (Months 9-14)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PHASE 3: GPU EMULATION                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Milestone 3.1: Vulkan Backend (4 weeks)                         │
│  ☐ Vulkan initialization for Android                             │
│  ☐ Swapchain and surface management                              │
│  ☐ Basic rendering pipeline                                      │
│  ☐ Command buffer management                                     │
│                                                                  │
│  Milestone 3.2: Command Processor (4 weeks)                      │
│  ☐ GPU ring buffer parsing                                       │
│  ☐ Register state machine                                        │
│  ☐ Draw command translation                                      │
│  ☐ State tracking and caching                                    │
│                                                                  │
│  Milestone 3.3: Shader Translator (8 weeks)                      │
│  ☐ Xenos microcode disassembler                                  │
│  ☐ Vertex shader translation                                     │
│  ☐ Pixel shader translation                                      │
│  ☐ Fetch shader handling                                         │
│  ☐ SPIR-V code generation                                        │
│  ☐ Shader caching system                                         │
│                                                                  │
│  Milestone 3.4: Texture System (4 weeks)                         │
│  ☐ Texture format conversion                                     │
│  ☐ Tiled texture detiling                                        │
│  ☐ Texture caching and streaming                                 │
│  ☐ Render target management                                      │
│                                                                  │
│  Milestone 3.5: eDRAM Emulation (4 weeks)                        │
│  ☐ Resolve operation implementation                              │
│  ☐ Tile-based rendering optimization                             │
│  ☐ MSAA handling                                                 │
│  ☐ Predicated tiling for large targets                           │
│                                                                  │
│  Deliverable: Render graphics in homebrew/simple games           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Phase 4: Audio & I/O (Months 15-17)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PHASE 4: AUDIO & I/O                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Milestone 4.1: Audio System (4 weeks)                           │
│  ☐ XMA decoder integration (FFmpeg)                              │
│  ☐ Audio mixing engine                                           │
│  ☐ AAudio output backend                                         │
│  ☐ Voice management (256 channels)                               │
│                                                                  │
│  Milestone 4.2: Input System (2 weeks)                           │
│  ☐ Android GameController API integration                        │
│  ☐ Touch overlay implementation                                  │
│  ☐ Keyboard/mouse support                                        │
│  ☐ Vibration feedback                                            │
│                                                                  │
│  Milestone 4.3: Storage (3 weeks)                                │
│  ☐ ISO/XISO mounting                                             │
│  ☐ GOD format support                                            │
│  ☐ Save data management                                          │
│  ☐ Content package handling                                      │
│                                                                  │
│  Milestone 4.4: Additional HLE (3 weeks)                         │
│  ☐ XAM implementation expansion                                  │
│  ☐ Networking stubs                                              │
│  ☐ Achievement/Profile stubs                                     │
│                                                                  │
│  Deliverable: Full emulator functionality                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Phase 5: Optimization (Months 18-21)

```
┌─────────────────────────────────────────────────────────────────┐
│                    PHASE 5: OPTIMIZATION                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Milestone 5.1: CPU Optimization (4 weeks)                       │
│  ☐ Profiling and hotspot identification                          │
│  ☐ JIT optimization passes                                       │
│  ☐ NEON intrinsic optimization                                   │
│  ☐ Block linking improvements                                    │
│                                                                  │
│  Milestone 5.2: GPU Optimization (4 weeks)                       │
│  ☐ Draw call batching                                            │
│  ☐ Shader optimization                                           │
│  ☐ Memory barrier optimization                                   │
│  ☐ Pipeline state caching                                        │
│                                                                  │
│  Milestone 5.3: Mobile-Specific (4 weeks)                        │
│  ☐ Thermal management                                            │
│  ☐ Dynamic resolution scaling                                    │
│  ☐ Frame pacing improvements                                     │
│  ☐ Power efficiency tuning                                       │
│                                                                  │
│  Milestone 5.4: Quality & Polish (4 weeks)                       │
│  ☐ Upscaling (FSR integration)                                   │
│  ☐ Anti-aliasing options                                         │
│  ☐ Color correction                                              │
│  ☐ UI/UX improvements                                            │
│                                                                  │
│  Deliverable: Playable performance on flagship devices           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Phase 6: Compatibility (Months 22-24+)

```
┌─────────────────────────────────────────────────────────────────┐
│                 PHASE 6: COMPATIBILITY                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Ongoing Tasks:                                                  │
│  ☐ Game-specific fixes and patches                               │
│  ☐ Missing HLE function implementation                           │
│  ☐ GPU accuracy improvements                                     │
│  ☐ Compatibility database                                        │
│  ☐ Community testing program                                     │
│  ☐ Regression testing suite                                      │
│                                                                  │
│  Target Compatibility Tiers:                                     │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ Tier 1: Perfect     - No issues, full speed                 │ │
│  │ Tier 2: Playable    - Minor issues, playable                │ │
│  │ Tier 3: In-Game     - Major issues, not fully playable      │ │
│  │ Tier 4: Intro       - Reaches menus/intro only              │ │
│  │ Tier 5: Nothing     - Crashes or doesn't load               │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Initial Target: 50+ games at Tier 2+ within 6 months of launch  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 8. Testing Strategy

### 8.1 Unit Testing

```
┌─────────────────────────────────────────────────────────────────┐
│                    UNIT TESTING                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  CPU Tests:                                                      │
│  • Individual PPC instruction verification                       │
│  • FPU precision tests                                           │
│  • VMX128 operation tests                                        │
│  • Exception handling tests                                      │
│                                                                  │
│  GPU Tests:                                                      │
│  • Shader translation correctness                                │
│  • Texture format conversion                                     │
│  • eDRAM resolve accuracy                                        │
│                                                                  │
│  System Tests:                                                   │
│  • HLE function behavior                                         │
│  • Memory management                                             │
│  • Thread synchronization                                        │
│                                                                  │
│  Framework: Google Test (gtest) with NDK support                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 Integration Testing

```
┌─────────────────────────────────────────────────────────────────┐
│                 INTEGRATION TESTING                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Test Homebrew Suite:                                            │
│  • Simple graphics tests                                         │
│  • Audio playback tests                                          │
│  • Input handling tests                                          │
│  • File system tests                                             │
│                                                                  │
│  Commercial Game Testing:                                        │
│  • Boot testing (reaches menu)                                   │
│  • Gameplay testing (playable)                                   │
│  • Completion testing (can finish)                               │
│  • Regression tracking                                           │
│                                                                  │
│  Performance Benchmarks:                                         │
│  • FPS tracking per game                                         │
│  • Frame time analysis                                           │
│  • Memory usage monitoring                                       │
│  • Thermal behavior logging                                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 8.3 Automated CI/CD

```yaml
# .github/workflows/build.yml concept
name: Build & Test

on: [push, pull_request]

jobs:
  build-android:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v3
        with:
          java-version: '17'
      - name: Setup Android SDK
        uses: android-actions/setup-android@v2
      - name: Build APK
        run: ./gradlew assembleDebug
      - name: Run Unit Tests
        run: ./gradlew test
      - name: Run Native Tests
        run: |
          cd native && mkdir build && cd build
          cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake ..
          make -j$(nproc) && ctest
```

---

## 9. Tools & Dependencies

### 9.1 Development Environment

```
┌─────────────────────────────────────────────────────────────────┐
│                 DEVELOPMENT SETUP                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  IDE:                                                            │
│  • Android Studio (latest) - Kotlin UI                           │
│  • CLion or VS Code - Native C++                                 │
│                                                                  │
│  Build Tools:                                                    │
│  • Android SDK 34+                                               │
│  • NDK r26+ (for latest ARM64 support)                           │
│  • CMake 3.22+                                                   │
│  • Ninja build system                                            │
│                                                                  │
│  Languages:                                                      │
│  • C++20 (with NDK support)                                      │
│  • Kotlin 1.9+                                                   │
│  • Assembly (ARM64)                                              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 9.2 Key Dependencies

```
┌─────────────────────────────────────────────────────────────────┐
│                   DEPENDENCIES                                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Graphics:                                                       │
│  • Vulkan SDK (Android native support)                           │
│  • SPIRV-Tools (shader manipulation)                             │
│  • glslang (shader compilation)                                  │
│  • VMA (Vulkan Memory Allocator)                                 │
│                                                                  │
│  Audio:                                                          │
│  • FFmpeg (XMA decoder, built with --enable-decoder=xma*)        │
│  • Oboe (high-performance audio, wraps AAudio)                   │
│                                                                  │
│  Utilities:                                                      │
│  • fmt (string formatting)                                       │
│  • spdlog (logging)                                              │
│  • toml++ (configuration)                                        │
│  • zstd (compression for save states)                            │
│  • xxhash (fast hashing for caches)                              │
│                                                                  │
│  Testing:                                                        │
│  • Google Test                                                   │
│  • Google Benchmark                                              │
│                                                                  │
│  UI (Kotlin):                                                    │
│  • Jetpack Compose                                               │
│  • Compose Material3                                             │
│  • Coil (image loading)                                          │
│  • Koin (dependency injection)                                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 9.3 Reference Materials

```
┌─────────────────────────────────────────────────────────────────┐
│               REFERENCE RESOURCES                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Existing Emulators (study, don't copy code):                    │
│  • Xenia (github.com/xenia-project/xenia) - Most complete        │
│  • Xemu (Xbox OG) - Similar architecture patterns                │
│                                                                  │
│  Documentation:                                                  │
│  • Free60.org wiki (Xbox 360 hardware docs)                      │
│  • IBM PowerPC manuals                                           │
│  • ATI/AMD shader documentation                                  │
│  • Xbox 360 SDK documentation (leaked)                           │
│                                                                  │
│  Technical Papers:                                               │
│  • "Xenos: Graphics for Xbox 360" (ATI whitepaper)               │
│  • "Unified Shader Architecture" papers                          │
│                                                                  │
│  Communities:                                                    │
│  • Xenia Discord                                                 │
│  • EmuDev Discord                                                │
│  • /r/emulation                                                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. Risk Assessment

### 10.1 Technical Risks

```
┌─────────────────────────────────────────────────────────────────┐
│                   RISK MATRIX                                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  HIGH IMPACT + HIGH PROBABILITY:                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Performance on mobile GPUs                                │ │
│  │   Mitigation: Early GPU prototyping, resolution scaling     │ │
│  │                                                             │ │
│  │ • VMX128 emulation overhead                                 │ │
│  │   Mitigation: Heavy NEON optimization, JIT specialization   │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  HIGH IMPACT + MEDIUM PROBABILITY:                               │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Shader translation accuracy                               │ │
│  │   Mitigation: Reference testing against Xenia, per-game fix │ │
│  │                                                             │ │
│  │ • Thermal throttling on sustained play                      │ │
│  │   Mitigation: Adaptive quality, frame rate targets          │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  MEDIUM IMPACT + HIGH PROBABILITY:                               │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ • Device fragmentation (different GPUs/drivers)             │ │
│  │   Mitigation: Multiple Vulkan backends, fallback paths      │ │
│  │                                                             │ │
│  │ • Memory constraints on 6GB devices                         │ │
│  │   Mitigation: Aggressive texture streaming, quality tiers   │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 10.2 Realistic Expectations

```
┌─────────────────────────────────────────────────────────────────┐
│              REALITY CHECK                                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  This is a multi-year, potentially multi-person project.         │
│                                                                  │
│  What's Achievable:                                              │
│  ✓ Basic homebrew and simple games on flagship 2024+ devices     │
│  ✓ Some commercial games at reduced settings                     │
│  ✓ Educational value and community contribution                  │
│                                                                  │
│  What's Extremely Difficult:                                     │
│  ✗ Full-speed Halo 3 on any mobile device                        │
│  ✗ Complete compatibility with all games                         │
│  ✗ Matching desktop Xenia performance                            │
│                                                                  │
│  Recommended Approach:                                           │
│  1. Start with a simpler target (2D games, older titles)         │
│  2. Build incrementally and test frequently                      │
│  3. Engage community early for testing and feedback              │
│  4. Consider contributing to Xenia instead for faster impact     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Appendix A: Initial Project Structure

```
360mu/
├── android/                    # Android application
│   ├── app/
│   │   ├── src/main/
│   │   │   ├── java/com/x360mu/
│   │   │   │   ├── MainActivity.kt
│   │   │   │   ├── ui/
│   │   │   │   │   ├── screens/
│   │   │   │   │   ├── components/
│   │   │   │   │   └── theme/
│   │   │   │   ├── core/
│   │   │   │   │   └── Emulator.kt        # JNI bridge
│   │   │   │   └── data/
│   │   │   └── res/
│   │   └── build.gradle.kts
│   └── build.gradle.kts
├── native/                     # Native C++ core
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── core/
│   │   │   ├── emulator.cpp
│   │   │   ├── emulator.h
│   │   │   └── config.h
│   │   ├── cpu/
│   │   │   ├── xenon/
│   │   │   │   ├── cpu.cpp
│   │   │   │   ├── cpu.h
│   │   │   │   ├── decoder.cpp
│   │   │   │   ├── interpreter.cpp
│   │   │   │   └── jit/
│   │   │   │       ├── jit.cpp
│   │   │   │       ├── ir.cpp
│   │   │   │       ├── arm64_emitter.cpp
│   │   │   │       └── optimizer.cpp
│   │   │   └── vmx128/
│   │   │       ├── vmx.cpp
│   │   │       └── neon_impl.cpp
│   │   ├── gpu/
│   │   │   ├── xenos/
│   │   │   │   ├── gpu.cpp
│   │   │   │   ├── command_processor.cpp
│   │   │   │   ├── shader_translator.cpp
│   │   │   │   └── texture_cache.cpp
│   │   │   └── vulkan/
│   │   │       ├── vulkan_backend.cpp
│   │   │       ├── pipeline_cache.cpp
│   │   │       └── memory_manager.cpp
│   │   ├── apu/
│   │   │   ├── audio.cpp
│   │   │   ├── xma_decoder.cpp
│   │   │   └── mixer.cpp
│   │   ├── kernel/
│   │   │   ├── kernel.cpp
│   │   │   ├── xex_loader.cpp
│   │   │   ├── hle/
│   │   │   │   ├── xboxkrnl.cpp
│   │   │   │   ├── xam.cpp
│   │   │   │   └── exports.cpp
│   │   │   └── filesystem/
│   │   │       ├── vfs.cpp
│   │   │       ├── iso_device.cpp
│   │   │       └── content_package.cpp
│   │   ├── memory/
│   │   │   ├── memory.cpp
│   │   │   ├── fastmem.cpp
│   │   │   └── heap.cpp
│   │   ├── input/
│   │   │   └── input.cpp
│   │   └── jni/
│   │       └── jni_bridge.cpp
│   ├── include/                # Public headers
│   ├── third_party/           # External dependencies
│   └── tests/
├── shaders/                    # GLSL/SPIR-V shaders
│   ├── blit.frag
│   ├── resolve.comp
│   └── detile.comp
├── docs/                       # Additional documentation
├── tools/                      # Development utilities
│   ├── xex_dump/
│   └── shader_debug/
├── .github/
│   └── workflows/
│       └── build.yml
├── README.md
├── LICENSE
└── DEVELOPMENT_PLAN.md         # This file
```

---

## Appendix B: Getting Started Commands

```bash
# Clone and setup
git clone https://github.com/yourname/360mu.git
cd 360mu

# Setup Android project
cd android
./gradlew assembleDebug

# Build native core (for testing on host)
cd ../native
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Build full APK with native code
cd ../../android
./gradlew assembleRelease
```

---

_This plan is a living document. Update it as the project evolves._

_Last Updated: December 2024_
_Version: 1.0_
