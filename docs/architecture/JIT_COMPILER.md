# JIT Compiler Architecture

## Overview

The JIT compiler (`native/src/cpu/jit/jit_compiler.cpp`) translates Xbox 360 PowerPC instructions to ARM64 machine code at runtime. It uses **fastmem optimization** to translate memory accesses into direct pointer arithmetic, avoiding slow virtual memory lookups for most operations.

## Key Files

| File | Purpose |
|------|---------|
| `jit_compiler.cpp` | Main JIT compiler, instruction translation |
| `jit_compiler.h` | JIT compiler class definition |
| `arm64_emitter.cpp` | ARM64 instruction encoding |
| `arm64_emitter.h` | ARM64 emitter class and register definitions |

## Memory Model

### Xbox 360 Memory Map

```
0x00000000 - 0x1FFFFFFF  (512MB)  Physical RAM - Main memory
0x20000000 - 0x7FBFFFFF  (1.5GB)  Physical RAM MIRRORS → maps to 0x00000000-0x1FFFFFFF
0x7FC00000 - 0x7FFFFFFF  (4MB)    GPU MMIO Physical - Command processor, registers
0x80000000 - 0x9FFFFFFF  (512MB)  Usermode Virtual → maps to physical 0x00000000-0x1FFFFFFF
0xA0000000 - 0xBFFFFFFF  (512MB)  Kernel Virtual - Requires MMIO/slow path
0xC0000000 - 0xC3FFFFFF  (64MB)   GPU Virtual Mapping - Framebuffer, textures
0xEC800000 - 0xECFFFFFF  (8MB)    Alternate GPU Virtual
```

### Fastmem Implementation

The emulator reserves a **4GB virtual address range** on the host but only **maps the first 512MB** to actual physical memory.

```cpp
// In memory.cpp
void* base = mmap(nullptr, 4GB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
mmap(base, 512MB, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

**Valid fastmem offsets:** `0x00000000` - `0x1FFFFFFF` (512MB)
**Invalid offsets:** `0x20000000` and above → SIGSEGV (unmapped memory)

### Address Translation Rule

All addresses destined for fastmem MUST be masked:
```cpp
physical_addr = guest_addr & 0x1FFFFFFF;
host_addr = fastmem_base + physical_addr;
```

### ⚠️ CRITICAL: Multi-Register Instructions

When generating code for instructions that access multiple memory locations (lmw, stmw, lvx, stvx, etc.), you MUST mask the **FULL address including offset** for each access:

```cpp
// ❌ WRONG - Offset can overflow past 512MB boundary!
emit.AND(X0, X0, 0x1FFFFFFF);      // Mask base only
emit.ADD(X0, X0, fastmem_base);
emit.LDR(X1, X0, offset);          // offset can push past 512MB!

// ✅ CORRECT - Mask full address for each access
emit.ADD(X3, X0, offset);          // Full address = base + offset
emit.AND(X3, X3, 0x1FFFFFFF);      // Mask full address
emit.ADD(X3, X3, fastmem_base);
emit.LDR(X1, X3);                  // Safe access
```

This bug caused crashes at exactly 0x20000000 when base was near end of 512MB range.

This mask ensures:
- `0x00000000` → `0x00000000` (unchanged)
- `0x20000000` → `0x00000000` (mirror wrapped)
- `0x80000000` → `0x00000000` (virtual to physical)
- `0x9FFFFFFF` → `0x1FFFFFFF` (virtual to physical)

## JIT Register Allocation

### ARM64 Register Usage

| Register | Purpose |
|----------|---------|
| X0-X15 | Scratch registers for computation |
| X16, X17 | Intra-procedure scratch (used for immediates, addresses) |
| X19 (CTX_REG) | ThreadContext pointer (callee-saved) |
| X20 | Reserved (formerly MEM_BASE, now unused) |
| X29, X30 | Frame pointer, link register |

### ThreadContext Structure

```cpp
struct ThreadContext {
    u64 gpr[32];           // General purpose registers
    u64 fpr[32];           // Floating point registers  
    u32 cr[8];             // Condition register fields
    u64 lr, ctr, xer;      // Special registers
    u64 time_base;         // Time base counter
    Memory* memory;        // Memory subsystem pointer
    // ... other fields
};
```

## Instruction Compilation Flow

### Block Compilation

```
1. compile_block(guest_pc)
   ├── emit_block_prologue()      # Save registers, setup CTX_REG
   ├── for each instruction:
   │   ├── decode_instruction()
   │   └── compile_XXX()          # Type-specific compilation
   ├── emit_block_epilogue()      # Update time_base, restore, RET
   └── flush_icache()             # Make code executable
```

### Memory Access Functions

There are **7 functions** that generate memory access code:

| Function | Purpose | Line ~Start |
|----------|---------|-------------|
| `compile_load()` | lwz, lbz, lhz, lfd, etc. | 1095 |
| `compile_store()` | stw, stb, sth, stfd, etc. | 1300 |
| `compile_load_multiple()` | lmw instruction | 1560 |
| `compile_store_multiple()` | stmw instruction | 1620 |
| `compile_atomic_load()` | lwarx, ldarx | 1690 |
| `compile_atomic_store()` | stwcx., stdcx. | 1760 |
| `compile_dcbz()` | Data cache block zero | 1850 |

## Address Routing Logic (V4 - Current)

All memory access functions use this routing logic:

```
Input: X0 = guest effective address

1. Check GPU Virtual (0xC0000000-0xC3FFFFFF)
   └── Yes → MMIO path

2. Check Alt GPU Virtual (0xEC800000-0xECFFFFFF)  
   └── Yes → MMIO path

3. Check Kernel Space (>= 0xA0000000)
   └── Yes → MMIO path

4. Check GPU MMIO Physical (0x7FC00000-0x7FFFFFFF)
   └── Yes → MMIO path

5. All other addresses:
   └── Apply mask (AND 0x1FFFFFFF) → Fastmem path
```

### Generated ARM64 Code Pattern (compile_store)

```asm
; === GPU Virtual Check ===
MOV     X16, #0xC0000000
CMP     X0, X16
B.CC    below_gpu_virt          ; if addr < 0xC0000000, skip
MOV     X16, #0xC4000000
CMP     X0, X16
B.CC    mmio_path               ; if addr < 0xC4000000, it's GPU virtual

below_gpu_virt:
; === Alt GPU Virtual Check ===
MOV     X16, #0xEC800000
CMP     X0, X16
B.CC    below_alt_gpu
MOV     X16, #0xED000000  
CMP     X0, X16
B.CC    mmio_path               ; if in 0xEC800000-0xECFFFFFF range

below_alt_gpu:
; === Kernel Check ===
MOV     X16, #0xA0000000
CMP     X0, X16
B.CS    mmio_path               ; if addr >= 0xA0000000

; === GPU MMIO Physical Check ===
MOV     X16, #0x7FC00000
CMP     X0, X16
B.CC    below_gpu_phys
MOV     X16, #0x80000000
CMP     X0, X16
B.CC    mmio_path               ; if in 0x7FC00000-0x7FFFFFFF range

below_gpu_phys:
; === Apply Mask ===
MOV     X16, #0x1FFFFFFF
AND     X0, X0, X16             ; X0 = addr & 0x1FFFFFFF

; Branch to fastmem path
B       fastmem_path

mmio_path:
; Call jit_mmio_write_XX(memory, addr, value)
LDR     X0, [CTX_REG, #memory_offset]
; ... setup args, call helper ...
B       done

fastmem_path:
; Reload value (may have been clobbered)
LDR     X1, [CTX_REG, #gpr_offset]
; Add fastmem base
MOV     X16, #fastmem_base
ADD     X0, X0, X16
; Byteswap and store
REV     W1, W1
STR     W1, [X0]

done:
```

## MMIO Helper Functions

Located at the top of `jit_compiler.cpp`:

```cpp
extern "C" void jit_mmio_write_u8(void* mem, GuestAddr addr, u8 value);
extern "C" void jit_mmio_write_u16(void* mem, GuestAddr addr, u16 value);
extern "C" void jit_mmio_write_u32(void* mem, GuestAddr addr, u32 value);
extern "C" void jit_mmio_write_u64(void* mem, GuestAddr addr, u64 value);

extern "C" u8  jit_mmio_read_u8(void* mem, GuestAddr addr);
extern "C" u16 jit_mmio_read_u16(void* mem, GuestAddr addr);
extern "C" u32 jit_mmio_read_u32(void* mem, GuestAddr addr);
extern "C" u64 jit_mmio_read_u64(void* mem, GuestAddr addr);
```

These are called when an address cannot use fastmem (kernel, GPU MMIO).

## Debug Tracing

### Compile-time Logging

```cpp
// In compile_store, first 3 stores are logged:
LOGI("Compiling store #%d: opcode=%d, ra=%d, simm=0x%04X", ...);

// Block dumps show first 256 bytes of generated code:
LOGI("Block at %08X code dump: ...", guest_pc);
```

### Runtime Tracing

```cpp
// jit_trace_store() - called for stores to addresses >= 0xA0000000
// Only logs GPU virtual (0xC0000000-0xCFFFFFFF) and GPU physical (0x7FC00000-0x7FFFFFFF)
```

## Known Issues / Debugging Notes

### SIGSEGV at Offset 0x20000000

**Symptom:** Crash with `fault addr = fastmem_base + 0x20000000`

**Root Cause:** Guest code accessing address 0x20000000 (or 0xA0000000, etc.) without proper masking.

**Verification:**
```
fault_offset = fault_addr - fastmem_base
if fault_offset == 0x20000000:
    # Mask not being applied!
    # Address 0x20000000 should become 0x00000000 after AND 0x1FFFFFFF
```

**Possible Causes:**
1. Address routing logic has a bug (branch to wrong path)
2. Mask instruction not being reached
3. Another code path bypassing the routing entirely
4. Register clobbering (X0 modified before mask applied)

### Debugging Checklist

1. **Verify build timestamp:** Ensure APK contains latest native code
2. **Check JIT logs:** `adb logcat -s 360mu-jit:*`
3. **Get fault address:** `adb logcat | grep SIGSEGV`
4. **Calculate offset:** `offset = fault_addr - fastmem_base`
5. **Decode guest instruction:** Check what PowerPC instruction caused crash
6. **Trace generated code:** Disassemble the block dump bytes

## ARM64 Emitter Notes

### Immediate Encoding Limitations

ARM64 logical immediates (AND, ORR, EOR) can only encode certain bitmask patterns. `0x1FFFFFFF` (29 consecutive 1s) may NOT be directly encodable.

```cpp
void ARM64Emitter::AND_imm(int rd, int rn, u64 imm) {
    if (encode_logical_imm_impl(imm, true, n, immr, imms)) {
        // Direct encoding
    } else {
        // Fallback: uses X16!
        MOV_imm(X16, imm);
        AND(rd, rn, X16);
    }
}
```

**IMPORTANT:** If `AND_imm` falls back to using X16, and X16 was already in use, this causes register clobbering!

Current code explicitly uses:
```cpp
emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
emit.AND(arm64::X0, arm64::X0, arm64::X16);
```
This is safe because we load X16 ourselves.

## Quick Reference: Compile Functions

### compile_load (line ~1095)
- Handles: lwz, lbz, lhz, lha, lfs, lfd, ld, indexed variants
- Flow: calc_ea → address routing → mask → fastmem load OR mmio call
- Update forms: Save EA to X3, restore to RA after load

### compile_store (line ~1300)  
- Handles: stw, stb, sth, stfs, stfd, std, indexed variants
- Has debug tracing for addresses >= 0xA0000000
- Flow: calc_ea → trace (optional) → address routing → mask → fastmem store OR mmio call

### compile_dcbz (line ~1850)
- Zeroes a 32-byte cache line
- Uses loop: 4 iterations × 8-byte stores
- Must mask address before loop

## TODO / Future Improvements

- [ ] Add runtime tracing for addresses 0x20000000-0x7FBFFFFF to catch mirror accesses
- [ ] Verify all 7 memory functions use identical routing logic
- [ ] Consider using a different scratch register (X17) to avoid X16 conflicts
- [ ] Add fastmem fault handler for graceful recovery instead of crash
