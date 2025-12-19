# 360μ Next Steps

## Current State (January 2025)

The emulator has made significant progress:

### ✅ Completed

| Component       | Status                                      |
| --------------- | ------------------------------------------- |
| Memory System   | 512MB RAM + fastmem working                 |
| XEX Loader      | Encryption, compression (LZX via libmspack) |
| CPU Interpreter | Full PowerPC instruction set + time_base    |
| CPU JIT         | ARM64 translation, ~80% coverage            |
| Threading       | 1:1 model with TLS, APCs, alertable waits   |
| Syscalls        | ~60 implemented, no unhandled errors        |
| VFS             | ISO mounting, file access                   |
| GPU Init        | Vulkan backend, test rendering              |
| GPU MMIO        | Virtual address routing fixed (see below)   |
| Shader Loops    | LOOP_START/LOOP_END/COND_EXEC implemented   |

### ✅ Recently Fixed: GPU MMIO Virtual Address Routing

**Previously:** GPU ring buffer registers were never being set (`rb_base=0, rb_size=0`).

**Root Cause:** Games write to GPU registers via virtual addresses (0xC0000000+), which must be translated to physical GPU MMIO space (0x7FC00000+). The code was incorrectly treating these as "kernel space" and bypassing MMIO handlers.

**Fix Applied (January 2025):**
1. `memory.cpp` - Updated `translate_address()` to handle GPU virtual mappings:
   - 0xC0000000-0xC3FFFFFF → 0x7FC00000+ (GPU MMIO)
   - 0xEC800000-0xECFFFFFF → 0x7FC00000+ (alternate mapping)
2. `memory.cpp` - Updated `is_mmio()` to recognize GPU virtual address ranges
3. `memory.cpp` - All read/write functions now translate address before MMIO lookup
4. `jit_compiler.cpp` - `compile_store()` now checks GPU virtual ranges before kernel space exclusion

### 🔄 Current Status: Needs Testing

The GPU MMIO fix should allow the ring buffer to receive configuration commands. The emulator needs to be rebuilt and tested with Call of Duty: Black Ops to verify the game progresses past the purple screen.

---

## Priority 1: Verify GPU Command Processing

### Task 1.1: Build and Test

1. Build the native library with the GPU MMIO fix
2. Install APK on device
3. Launch Call of Duty: Black Ops
4. Check logs for ring buffer activity:

```bash
adb logcat -s 360mu:* 360mu-gpu:* | grep "rb_base\|ring buffer\|write_register"
```

**Expected:** Ring buffer registers should now show non-zero values.

### Task 1.2: Command Processor Implementation

If ring buffer is now configured, implement PM4 command parsing:

1. Parse PM4 command packets
2. Execute draw commands
3. Manage render targets
4. Handle synchronization primitives

**Files:**
- `native/src/gpu/xenos/gpu.cpp` - Command processing
- `native/src/gpu/vulkan/vulkan_backend.cpp` - Vulkan rendering

---

## Priority 2: Additional Syscall Implementation

### Task 2.1: Identify Missing Syscalls

Run the game and check for:

```bash
adb logcat -d | grep "UNIMPLEMENTED syscall"
```

Common missing syscalls for games:

- File I/O (NtCreateFile, NtReadFile, etc.)
- Memory mapping (NtAllocateVirtualMemory)
- Timer/wait functions (KeDelayExecutionThread)

### Task 2.2: Implement Critical Syscalls

Based on what games need, prioritize:

| Ordinal | Function          | Priority |
| ------- | ----------------- | -------- |
| Various | File I/O syscalls | High     |
| Various | Timer syscalls    | High     |
| Various | Memory syscalls   | Medium   |
| Various | Object syscalls   | Medium   |

---

## Priority 3: Shader Translation

### Task 3.1: Xenos Shader Analysis

The Xbox 360's Xenos GPU uses a custom shader format:

- Unified shader architecture
- Xbox-specific instructions
- Need to translate to SPIR-V for Vulkan

### Task 3.2: Shader Translator

Implement shader translation pipeline:

1. Parse Xenos shader bytecode
2. Translate to intermediate representation
3. Generate SPIR-V for Vulkan
4. Handle shader caching

**Reference:** Xenia's shader translator

---

## Priority 4: Audio Integration

### Task 4.1: XMA Decoder Connection

The XMA decoder exists but isn't receiving data:

- Implement XMA context management
- Route audio data from game to decoder
- Output decoded audio to Android audio system

### Task 4.2: Audio Timing

Xbox 360 audio is timing-sensitive:

- 48kHz sample rate
- Proper buffer management
- Sync with video output

---

## Priority 5: Input and Controllers

### Task 5.1: Controller Emulation

Implement XInput-style controller:

- Touch screen controls
- Physical controller support
- Rumble/vibration

### Task 5.2: Keyboard/Mouse

For games that support it:

- Mouse input for FPS games
- Keyboard mapping

---

## Technical Debt

### Cleanup Tasks

1. ~~**Remove debug logging** - Many hypothesis logs are still in code~~ ✅ Completed
2. **Code organization** - Some fixes are inline, should be refactored
3. **Error handling** - Add proper error messages and recovery
4. **Performance profiling** - Identify bottlenecks

### Documentation

1. ✅ Updated README.md
2. ✅ Updated THREADING_MODEL.md
3. ✅ Updated SPIN_LOOP_WORKER_THREAD_ISSUE.md
4. ✅ Created this NEXT_STEPS.md
5. Consider documenting GPU architecture

---

## Testing Strategy

### Game Compatibility

Test with multiple games to find common issues:

| Game                    | Status               | Notes                             |
| ----------------------- | -------------------- | --------------------------------- |
| Call of Duty: Black Ops | Boots, purple screen | GPU MMIO fix applied, needs test  |
| (Other games)           | Untested             | Need testing                      |

### Regression Testing

After each major change:

1. Verify Call of Duty still boots
2. Check for new UNIMPLEMENTED syscalls
3. Monitor CPU/memory usage

---

## Resources

### Reference Implementations

- [Xenia](https://github.com/xenia-project/xenia) - Most complete Xbox 360 emulator
- [Xenia Canary](https://github.com/xenia-canary/xenia-canary) - Active fork with improvements

### Documentation

- [Xbox 360 Architecture](https://www.copetti.org/writings/consoles/xbox-360/)
- [Free60 Wiki](https://free60.org/)
- [Xbox Dev Wiki](https://xboxdevwiki.net/)

---

## Quick Reference: Debug Commands

```bash
# View all emulator logs
adb logcat -s 360mu:* 360mu-cpu:* 360mu-jit:* 360mu-kernel:* 360mu-gpu:*

# Check GPU status
adb logcat -d | grep "ring buffer\|GPU\|rb_base"

# Check for unimplemented syscalls
adb logcat -d | grep "UNIMPLEMENTED"

# Check thread activity
adb logcat -d | grep "thread\|Thread"

# Read debug log file
adb shell run-as com.x360mu cat /data/data/com.x360mu/files/debug.log | tail -50

# Clear debug log
adb shell run-as com.x360mu rm /data/data/com.x360mu/files/debug.log
```

---

## Timeline Estimate

| Priority | Task               | Estimate  | Status         |
| -------- | ------------------ | --------- | -------------- |
| P1       | GPU Ring Buffer    | 1-2 weeks | ✅ Fix applied |
| P1       | Command Processor  | 2-4 weeks | Next priority  |
| P2       | Missing Syscalls   | Ongoing   | As needed      |
| P3       | Shader Translation | 4-8 weeks | Basic done     |
| P4       | Audio Integration  | 2-3 weeks | Pending        |
| P5       | Input/Controllers  | 1-2 weeks | Pending        |

**Next Step:** Build and test with Call of Duty: Black Ops to verify GPU commands are now being received.
