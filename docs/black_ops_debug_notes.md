# Call of Duty: Black Ops - Debugging Notes

## Summary
Black Ops fails to boot due to initialization issues. The game gets stuck in two distinct phases that suggest missing or incomplete emulation features.

## Issue 1: Invalid Memset Call (PC: 0x825FB308)

### Symptoms
- Game repeatedly calls a memset-like function at address `0x825FB308`
- Destination pointer in r3 is invalid: `0x9` (should be a valid memory address)
- Called from: `LR=0x824D35D4`
- Execution stuck in tight loop with `CTR` counting down from ~0xFFFFF983

### Register State (First Hit)
```
r0-r7:   0xC 0x7003FA70 0x0 0x9 0x0 0xC1 0x67DC 0x1A0
r8-r15:  0x1 0x40000 0x1C 0x8 0x82605BC8 0x900000 0x0 0x0
r30-r31: 0x9 0x7003FA70
LR:      0x824D35D4
CTR:     0xFFFFF983
r13(PCR): 0x00900000
PCR[0](TLS ptr): 0x00800000
```

### Analysis
- PCR and TLS are correctly initialized (r13=0x900000, TLS=0x800000)
- The invalid pointer (0x9) suggests a data structure wasn't properly allocated or initialized
- This is likely a symptom of missing initialization code, not the root cause

### Workaround Implemented
```cpp
// In cpu.cpp execute_with_context()
if (stuck_pc_count >= 10 && external_ctx.gpr[3] < 0x1000) {
    LOGI("WORKAROUND: Memset loop with invalid ptr - returning success");
    external_ctx.pc = external_ctx.lr;  // Return to caller
    external_ctx.ctr = 0;
    stuck_pc_count = 0;
}
```

**Result**: Workaround succeeds, execution continues, but game gets stuck at next issue.

---

## Issue 2: Syscall Spin Loop (PC: 0x82612288)

### Symptoms
After the memset workaround:
- Game execution continues at ~20 FPS
- PC stuck spinning at address `0x82612288` (syscall stub area: 0x82612xxx)
- Called from: `LR=0x824CF3A4`
- time_base continues incrementing (CPU is running)
- **No draw commands issued** - game never reaches rendering stage

### Analysis
- Address `0x82612xxx` is in the import thunk area (syscall stubs)
- The game is waiting for a syscall to return a specific value or flag
- This is a **spin-wait loop** checking for completion of an async operation
- Likely waiting for:
  - Thread synchronization primitive (event, semaphore, critical section)
  - Hardware initialization completion
  - File I/O operation
  - Graphics subsystem initialization

### Root Cause
The invalid memset and subsequent spin loop both indicate that **Black Ops requires initialization or synchronization features that aren't fully implemented** in the emulator. The game expects certain kernel objects or hardware states to be set up before it can proceed.

---

## Attempted Fixes

### Fix 1: Return to Caller (Immediate)
```cpp
external_ctx.pc = external_ctx.lr;  // Jump to LR=0x824D35D4
```
**Result**: Caller immediately calls memset again in a loop

### Fix 2: Skip Call Instruction (LR+4)
```cpp
external_ctx.pc = external_ctx.lr + 4;  // Skip the BL instruction
```
**Result**: Execution continues but leads to syscall spin loop at 0x82612288

### Fix 3: Stack Walking to Skip Caller's Caller
```cpp
// Read PowerPC stack back-chain to find caller's caller
GuestAddr prev_frame = memory_->read_u32(stack_ptr + 0);
GuestAddr saved_lr = memory_->read_u32(prev_frame + 4);
external_ctx.pc = saved_lr;
```
**Result**: Stack frame's saved LR was 0x00000000 (invalid), fell back to LR+4

---

## Versions Tested

| Build | Version | Changes | Result |
|-------|---------|---------|--------|
| 2 | 0.1.1-dev | Initial PCR/TLS fixes | Stuck at memset |
| 3 | 0.1.2-dev | Stack walking workaround | Stuck at memset |
| 4 | 0.1.3-dev | Improved stack walking | Stuck at memset |
| 5 | 0.1.4-dev | Simple return workaround | Stuck at syscall 0x82612288 |

---

## Conclusion

**Black Ops is too complex for the current emulator state.** It requires:
1. More complete kernel HLE (threading, synchronization)
2. Proper hardware initialization sequences
3. Additional syscalls implemented

**Recommendation**: Test with simpler games (XBLA titles) to validate core emulation functionality before tackling complex AAA titles.

---

## Performance Notes

When running (even while stuck):
- CPU execution: ~20 FPS equivalent
- JIT compilation: Working
- Memory access: Fast (fastmem optimization functional)
- No crashes: Emulator is stable, just incomplete

---

## Files Modified

- [native/src/cpu/xenon/cpu.cpp](../native/src/cpu/xenon/cpu.cpp): Added stuck PC detection and workarounds
- [native/src/kernel/xthread.cpp](../native/src/kernel/xthread.cpp): Fixed PCR/TLS initialization
- [native/src/cpu/xenon/threading.cpp](../native/src/cpu/xenon/threading.cpp): Fixed PCR/TLS initialization
- [android/app/build.gradle.kts](../android/app/build.gradle.kts): Added version tracking

---

## Next Steps

1. ✅ Document findings
2. ⏭️ Add XBLA support (simpler games for testing)
3. ⏭️ Test with XBLA titles (Geometry Wars, Castle Crashers, etc.)
4. ⏭️ Implement missing syscalls as needed for simpler games
5. ⏭️ Return to Black Ops once core functionality is proven
