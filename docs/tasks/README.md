# 360μ Development Tasks

## Quick Reference

| Task              | File                           | Priority    | Status         | Dependencies |
| ----------------- | ------------------------------ | ----------- | -------------- | ------------ |
| File System       | `TASK_01_FILE_SYSTEM.md`       | ✅ DONE     | Complete       | None         |
| Vulkan Backend    | `TASK_02_VULKAN_BACKEND.md`    | ✅ DONE     | Initialized    | None         |
| Shader Translator | `TASK_03_SHADER_TRANSLATOR.md` | 🔴 CRITICAL | Not Started    | Task 02      |
| Command Processor | `TASK_04_COMMAND_PROCESSOR.md` | 🔴 CRITICAL | Partial        | Task 02      |
| JIT Codegen       | `TASK_05_JIT_CODEGEN.md`       | ✅ DONE     | ~80% coverage  | None         |
| Kernel File I/O   | `TASK_06_KERNEL_FILE_IO.md`    | ✅ DONE     | Working        | Task 01      |
| Kernel Threading  | `TASK_07_KERNEL_THREADING.md`  | ✅ DONE     | 1:1 model      | None         |
| XMA Audio         | `TASK_08_XMA_AUDIO.md`         | 🟡 MEDIUM   | Framework only | None         |

---

## Current Focus: GPU Command Processing

The threading system is now working (1:1 model). The current blocker is GPU command processing:

1. Game boots and executes syscalls correctly
2. GPU is initialized with Vulkan
3. **Ring buffer never receives commands** ← Current issue
4. Game stuck in secondary polling loop

See [NEXT_STEPS.md](../NEXT_STEPS.md) for detailed roadmap.

---

## Task Descriptions

### ✅ TASK_01: File System (COMPLETE)

**What:** ISO 9660 and STFS file system mounting  
**Status:** Working - games load from ISO files  
**Scope:** `native/src/vfs/`

### ✅ TASK_02: Vulkan Backend (COMPLETE)

**What:** Initialize Vulkan, create swapchain, basic rendering  
**Status:** Working - test render produces purple screen  
**Scope:** `native/src/gpu/vulkan/`

### 🔴 TASK_03: Shader Translator (NOT STARTED)

**What:** Translate Xenos shader microcode to SPIR-V  
**Why:** No shaders = no graphics  
**Scope:** `native/src/gpu/xenos/shader_translator.cpp`

### 🔴 TASK_04: Command Processor (PARTIAL)

**What:** Parse GPU PM4 command buffers, dispatch draws  
**Status:** Code exists but ring buffer not receiving commands  
**Scope:** `native/src/gpu/xenos/command_processor.cpp`

### ✅ TASK_05: JIT Codegen (MOSTLY COMPLETE)

**What:** Generate ARM64 machine code from PowerPC  
**Status:** ~80% instruction coverage, syscalls working  
**Scope:** `native/src/cpu/jit/`

### ✅ TASK_06: Kernel File I/O (COMPLETE)

**What:** NtCreateFile, NtReadFile, etc. HLE  
**Status:** Working - games can read files  
**Scope:** `native/src/kernel/hle/xboxkrnl_io.cpp`

### ✅ TASK_07: Kernel Threading (COMPLETE)

**What:** Thread creation, events, semaphores, synchronization  
**Status:** 1:1 threading model implemented and working  
**Scope:** `native/src/cpu/xenon/threading.cpp`

### 🟡 TASK_08: XMA Audio (PARTIAL)

**What:** Decode XMA compressed audio to PCM  
**Status:** Decoder exists but not receiving data  
**Scope:** `native/src/apu/xma_decoder.cpp`

---

## Progress Tracking

| Task                 | Status         | Notes                      |
| -------------------- | -------------- | -------------------------- |
| 01 File System       | ✅ Complete    | ISO mounting works         |
| 02 Vulkan Backend    | ✅ Complete    | Test rendering works       |
| 03 Shader Translator | ⬜ Not Started | Needs ring buffer first    |
| 04 Command Processor | 🟡 Partial     | Ring buffer not configured |
| 05 JIT Codegen       | ✅ Complete    | ~80% coverage              |
| 06 Kernel File I/O   | ✅ Complete    | Working                    |
| 07 Kernel Threading  | ✅ Complete    | 1:1 model works            |
| 08 XMA Audio         | 🟡 Framework   | Needs integration          |

---

## Critical Path to First Render

To show something other than purple screen:

1. ✅ **Task 01** - Mount game ISO
2. ✅ **Task 06** - Read default.xex
3. ✅ **Task 07** - Threading works
4. ✅ **Task 02** - Initialize Vulkan
5. 🔴 **Task 04** - Ring buffer reception ← **CURRENT BLOCKER**
6. 🔴 **Task 03** - Translate shaders

---

## File Ownership

| Task | Owns                                              |
| ---- | ------------------------------------------------- |
| 01   | `vfs/*`                                           |
| 02   | `gpu/vulkan/*`                                    |
| 03   | `gpu/xenos/shader_translator.*`                   |
| 04   | `gpu/xenos/command_processor.*`                   |
| 05   | `cpu/jit/*`                                       |
| 06   | `kernel/hle/xboxkrnl_io.cpp`                      |
| 07   | `cpu/xenon/threading.*`, `kernel/hle/*threading*` |
| 08   | `apu/xma_decoder.*`                               |

**Shared (read-only for most tasks):**

- `cpu/xenon/cpu.h` - CPU types
- `memory/memory.h` - Memory interface
- `kernel/kernel.h` - Kernel types
- `types.h` - Common types

---

_Last updated: December 2024_
