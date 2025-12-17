# 360μ Development Tasks

## Quick Reference

| Task | File | Priority | Time Est. | Dependencies |
|------|------|----------|-----------|--------------|
| File System | `TASK_01_FILE_SYSTEM.md` | 🔴 CRITICAL | 2-4 weeks | None |
| Vulkan Backend | `TASK_02_VULKAN_BACKEND.md` | 🔴 CRITICAL | 2-3 weeks | None |
| Shader Translator | `TASK_03_SHADER_TRANSLATOR.md` | 🔴 CRITICAL | 6-10 weeks | Task 02 |
| Command Processor | `TASK_04_COMMAND_PROCESSOR.md` | 🔴 CRITICAL | 3-4 weeks | Task 02 |
| JIT Codegen | `TASK_05_JIT_CODEGEN.md` | 🔴 CRITICAL | 6-8 weeks | None |
| Kernel File I/O | `TASK_06_KERNEL_FILE_IO.md` | 🔴 CRITICAL | 2-3 weeks | Task 01 |
| Kernel Threading | `TASK_07_KERNEL_THREADING.md` | 🟡 HIGH | 3-4 weeks | None |
| XMA Audio | `TASK_08_XMA_AUDIO.md` | 🟡 MEDIUM | 2-3 weeks | None |

---

## Task Descriptions

### 🔴 TASK_01: File System
**What:** ISO 9660 and STFS file system mounting  
**Why:** Games can't load without reading their data files  
**Scope:** `native/src/kernel/filesystem/`

### 🔴 TASK_02: Vulkan Backend
**What:** Initialize Vulkan, create swapchain, basic rendering  
**Why:** No GPU = black screen  
**Scope:** `native/src/gpu/vulkan/`

### 🔴 TASK_03: Shader Translator  
**What:** Translate Xenos shader microcode to SPIR-V  
**Why:** No shaders = no graphics  
**Scope:** `native/src/gpu/xenos/shader_translator.cpp`

### 🔴 TASK_04: Command Processor
**What:** Parse GPU PM4 command buffers, dispatch draws  
**Why:** Bridge between game and GPU  
**Scope:** `native/src/gpu/xenos/command_processor.cpp`

### 🔴 TASK_05: JIT Codegen
**What:** Generate ARM64 machine code from PowerPC  
**Why:** Interpreter is ~100x too slow for gameplay  
**Scope:** `native/src/cpu/jit/`

### 🔴 TASK_06: Kernel File I/O
**What:** NtCreateFile, NtReadFile, etc. HLE  
**Why:** Games call these to load assets  
**Scope:** `native/src/kernel/hle/xboxkrnl_io.cpp`

### 🟡 TASK_07: Kernel Threading
**What:** Thread creation, events, semaphores, critical sections  
**Why:** Multi-threaded games need synchronization  
**Scope:** `native/src/kernel/hle/xboxkrnl_threading.cpp`

### 🟡 TASK_08: XMA Audio
**What:** Decode XMA compressed audio to PCM  
**Why:** No audio decoding = silence  
**Scope:** `native/src/apu/xma_decoder.cpp`

---

## Parallel Work Strategy

These tasks can be worked on **simultaneously** without conflicts:

### Group A (No Dependencies)
- Task 01: File System
- Task 02: Vulkan Backend  
- Task 05: JIT Codegen
- Task 07: Kernel Threading
- Task 08: XMA Audio

### Group B (Depends on Group A)
- Task 03: Shader Translator (needs Task 02)
- Task 04: Command Processor (needs Task 02)
- Task 06: Kernel File I/O (needs Task 01)

---

## How to Use These Tasks

### For Human Developers
1. Pick a task from Group A
2. Read the corresponding `TASK_XX_*.md` file
3. Follow the implementation guide
4. Write tests as specified
5. Don't touch files listed in "Do NOT Touch"

### For AI Agents
When assigning to an AI agent, use:

```
Please implement the task described in:
docs/tasks/TASK_XX_NAME.md

Key constraints:
- Only modify files in the specified scope
- Do not touch files in other tasks
- Write tests as described
- Follow the existing code style
```

---

## Progress Tracking

Update this section as tasks complete:

| Task | Status | Assignee | Notes |
|------|--------|----------|-------|
| 01 File System | ⬜ Not Started | | |
| 02 Vulkan Backend | ⬜ Not Started | | |
| 03 Shader Translator | ⬜ Not Started | | |
| 04 Command Processor | ⬜ Not Started | | |
| 05 JIT Codegen | ⬜ Not Started | | |
| 06 Kernel File I/O | ⬜ Not Started | | |
| 07 Kernel Threading | ⬜ Not Started | | |
| 08 XMA Audio | ⬜ Not Started | | |

---

## Critical Path to Boot

To show the Activision logo (first milestone):

1. ✅ **Task 01** - Mount game ISO
2. ✅ **Task 06** - Read default.xex  
3. ✅ **Task 02** - Initialize Vulkan
4. ✅ **Task 04** - Parse GPU commands
5. ✅ **Task 03** - Translate shaders (basic)

Minimum viable for boot: Tasks 01 → 06 → 02 → 04 → 03 (partial)

---

## File Ownership

To prevent conflicts, each task owns specific files:

| Task | Owns |
|------|------|
| 01 | `kernel/filesystem/*` |
| 02 | `gpu/vulkan/*` |
| 03 | `gpu/xenos/shader_translator.*` |
| 04 | `gpu/xenos/command_processor.*` |
| 05 | `cpu/jit/*` |
| 06 | `kernel/hle/xboxkrnl_io.cpp` |
| 07 | `kernel/hle/xboxkrnl_threading.cpp`, `kernel/threading.*` |
| 08 | `apu/xma_decoder.*` |

**Shared (read-only for most tasks):**
- `cpu/xenon/cpu.h` - CPU types
- `memory/memory.h` - Memory interface
- `kernel/kernel.h` - Kernel types
- `x360mu/types.h` - Common types

---

*Last updated: December 2024*
