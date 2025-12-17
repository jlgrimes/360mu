# Parallel Task Instructions for 360μ

## Overview

These task documents are designed for parallel AI agent work on the 360μ Xbox 360 emulator. Each task is **independent** and can be worked on simultaneously.

## Task List

| Task | File | Priority | Dependencies |
|------|------|----------|--------------|
| **GPU/Vulkan** | `TASK_GPU_VULKAN.md` | HIGH | Vulkan SDK |
| **JIT Compiler** | `TASK_JIT_COMPILER.md` | HIGH | ARM64 device for testing |
| **Audio/XMA** | `TASK_AUDIO_XMA.md` | MEDIUM | FFmpeg |
| **Kernel HLE** | `TASK_KERNEL_HLE.md` | HIGH | None |
| **Android Build** | `TASK_ANDROID_BUILD.md` | HIGH | Android Studio |
| **CPU Instructions** | `TASK_CPU_INSTRUCTIONS.md` | HIGH | None |

## Recommended Parallelization

### Minimum 3 Agents
1. **Agent A**: `TASK_JIT_COMPILER.md` + `TASK_CPU_INSTRUCTIONS.md` (CPU-focused)
2. **Agent B**: `TASK_GPU_VULKAN.md` (GPU-focused)
3. **Agent C**: `TASK_KERNEL_HLE.md` + `TASK_AUDIO_XMA.md` (System-focused)

### Optimal 6 Agents
One agent per task file.

## Project Structure

```
360mu/
├── native/                 # C++ emulator core
│   ├── CMakeLists.txt
│   ├── include/x360mu/     # Public headers
│   ├── src/
│   │   ├── core/           # Main emulator
│   │   ├── cpu/
│   │   │   ├── xenon/      # CPU decoder/interpreter
│   │   │   ├── vmx128/     # SIMD unit
│   │   │   └── jit/        # JIT compiler (Task: JIT_COMPILER)
│   │   ├── gpu/
│   │   │   ├── xenos/      # GPU emulation (Task: GPU_VULKAN)
│   │   │   └── vulkan/     # Vulkan backend
│   │   ├── apu/            # Audio (Task: AUDIO_XMA)
│   │   ├── kernel/         # HLE (Task: KERNEL_HLE)
│   │   │   └── hle/
│   │   ├── memory/
│   │   └── jni/            # Android bridge (Task: ANDROID_BUILD)
│   ├── tests/              # Unit tests
│   └── tools/              # Test utilities
├── android/                # Android app (Task: ANDROID_BUILD)
│   ├── app/
│   │   └── src/main/java/com/x360mu/
│   └── build.gradle.kts
└── docs/
    ├── DEVELOPMENT_PLAN.md # Full technical plan
    ├── BLACK_OPS_COMPATIBILITY.md
    └── tasks/              # These task files
```

## Building the Project

### Host (macOS/Linux)
```bash
cd native/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
./x360mu_tests  # Run tests
```

### Android
```bash
cd android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## Key Headers to Know

Before starting any task, read:

1. **`native/include/x360mu/types.h`** - Common types (u8, u16, u32, u64, s8, s16, etc.)
2. **`native/include/x360mu/emulator.h`** - Main emulator interface
3. **`native/src/cpu/xenon/cpu.h`** - CPU types (ThreadContext, DecodedInst)
4. **`native/src/memory/memory.h`** - Memory interface

## Communication Between Tasks

### GPU ↔ CPU
- GPU reads from memory mapped at `0x7FC00000` (command buffer)
- CPU writes PM4 packets via `Gpu::write_register()`

### Audio ↔ CPU
- Audio contexts at `0x7FE00000+`
- CPU triggers via `Apu::write_register()`

### Kernel ↔ CPU
- Kernel syscalls via supervisor call instruction (sc)
- HLE intercepts at specific addresses

## Integration Checkpoints

When your task reaches these milestones, verify:

1. **GPU**: Can create Vulkan context → notify Android task
2. **JIT**: Basic block compilation → test with CPU task
3. **Audio**: XMA decode works → test with kernel file I/O
4. **Kernel**: Thread creation → test with CPU multi-threading
5. **Android**: Native lib loads → integrate with all

## Testing Reference

All tests are in `native/tests/`:
```
test_decoder.cpp      - PowerPC decoder tests
test_interpreter.cpp  - Instruction execution tests
test_vmx128.cpp       - VMX128 SIMD tests
test_memory.cpp       - Memory system tests
test_xex_loader.cpp   - XEX file loading tests
```

## External References

- **Xenia** (Reference implementation): https://github.com/xenia-project/xenia
- **PowerPC ISA**: Search "PowerPC User Instruction Set Architecture"
- **Vulkan Spec**: https://www.khronos.org/vulkan/
- **Xbox 360 Docs**: XenonWiki, FreeXDB

## Questions?

If you hit a blocker, the most likely issue is:
1. Missing include path → Check CMakeLists.txt
2. Missing type definition → Check types.h
3. API mismatch → Check the relevant .h file

Good luck! 🎮

