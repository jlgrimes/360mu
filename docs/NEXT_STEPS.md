# 360μ Next Steps

## Current State (December 2024)

The emulator has made significant progress:

### ✅ Completed

| Component       | Status                               |
| --------------- | ------------------------------------ |
| Memory System   | 512MB RAM + fastmem working          |
| XEX Loader      | Encryption, compression, imports     |
| CPU Interpreter | Full PowerPC instruction set         |
| CPU JIT         | ARM64 translation, ~80% coverage     |
| Threading       | 1:1 model with TLS, context sync     |
| Syscalls        | ~60 implemented, no unhandled errors |
| VFS             | ISO mounting, file access            |
| GPU Init        | Vulkan backend, test rendering       |

### 🔄 Current Blocker

**GPU Ring Buffer Not Receiving Commands**

The game initializes but the GPU command processor never receives ring buffer commands:

- `rb_base=0, rb_size=0` in every `process_commands()` call
- Game is stuck in a secondary polling loop at PC `0x825FB308`
- VBlanks and frames are being presented (purple test color)

---

## Priority 1: GPU Command Processing

### Task 1.1: Debug Ring Buffer Setup

The game should write to GPU MMIO registers to configure the ring buffer:

- `CP_RB_BASE` (0x0700) - Ring buffer base address
- `CP_RB_CNTL` (0x0701) - Ring buffer size/control
- `CP_RB_WPTR` (0x070E) - Write pointer

**Investigation needed:**

1. Is the game writing to GPU MMIO addresses (0x7FC00000+)?
2. Are MMIO writes being routed through the JIT MMIO helper?
3. Is the GPU MMIO handler receiving these writes?

**Files:**

- `native/src/gpu/xenos/gpu.cpp` - `write_register()` handles MMIO
- `native/src/cpu/jit/jit_compiler.cpp` - MMIO write routing
- `native/src/memory/memory.cpp` - MMIO handler registration

### Task 1.2: Trace Game Initialization

Add logging to understand what the game is doing before it gets stuck:

- What syscalls are being made?
- What memory addresses are being polled?
- Is there another event the game is waiting for?

### Task 1.3: Implement Command Processor

Once ring buffer is configured:

1. Parse PM4 command packets
2. Execute draw commands
3. Manage render targets
4. Handle synchronization primitives

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

1. **Remove debug logging** - Many hypothesis logs are still in code
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

| Game                    | Status               | Notes                 |
| ----------------------- | -------------------- | --------------------- |
| Call of Duty: Black Ops | Boots, purple screen | GPU ring buffer issue |
| (Other games)           | Untested             | Need testing          |

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

| Priority | Task               | Estimate  |
| -------- | ------------------ | --------- |
| P1       | GPU Ring Buffer    | 1-2 weeks |
| P1       | Command Processor  | 2-4 weeks |
| P2       | Missing Syscalls   | Ongoing   |
| P3       | Shader Translation | 4-8 weeks |
| P4       | Audio Integration  | 2-3 weeks |
| P5       | Input/Controllers  | 1-2 weeks |

**Goal:** First game rendering something other than purple screen within 1-2 months.
