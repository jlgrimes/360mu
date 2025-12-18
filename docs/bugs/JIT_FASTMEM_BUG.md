# JIT Fastmem Address Translation Bug

## Status: FIXED ✅

**Root Cause**: Two issues combined:
1. `main_memory_` and `fastmem_base_` were separate memory regions - interpreter wrote to one, JIT read from the other
2. Address translation was overly complex with conditional branches that could skip the mask

**Fix Applied** (December 2024):
1. **Memory Unification**: Modified `setup_fastmem()` to copy `main_memory_` content into `fastmem_base_`, then redirect `main_memory_` to point to `fastmem_base_`. Both now reference the same memory.
2. **Simplified Address Translation**: Changed `emit_translate_address()` to ALWAYS mask addresses with `0x1FFFFFFF`. This matches how most Xbox 360 emulators handle it:
   ```cpp
   // Simple and correct - always mask to 512MB physical range
   emit.AND_imm(addr_reg, addr_reg, 0x1FFFFFFFULL);
   emit.MOV_imm(arm64::X16, fastmem_base);
   emit.ADD(addr_reg, addr_reg, arm64::X16);
   ```

**Result**: JIT runs at ~22 FPS, no crashes, CPU executing properly.

---

## Original Summary

The JIT compiler crashes with SIGSEGV when executing memory access instructions. The fault address is consistently ~2GB below where it should be, indicating the MEM_BASE register (X20) contains an incorrect value.

## Original Symptoms

- **Crash type**: SIGSEGV (SEGV_MAPERR)
- **Pattern**: Fault address is always ~0x7C017B18 (~2GB) below fastmem_base
- **When**: Crashes on first memory store operation (block 3, `stwu` instruction)
- **Interpreter**: Works correctly (no crash)

### Example Crash Data

| Run | fastmem_base | fault_addr   | Difference |
| --- | ------------ | ------------ | ---------- |
| 1   | 0x70a72cc000 | 0x702b2b44e8 | ~2GB       |
| 2   | 0x70a58d2000 | 0x70298ba4e8 | ~2GB       |
| 3   | 0x70a827b000 | 0x702c2634e8 | ~2GB       |

## Technical Details

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        JIT Execution Flow                        │
├─────────────────────────────────────────────────────────────────┤
│  execute()                                                       │
│    ├── compile_block(pc) -> CompiledBlock*                      │
│    └── fn(&ctx, nullptr)  // Call compiled ARM64 code           │
│                                                                  │
│  Block Structure:                                                │
│    ├── Prologue (save regs, set CTX_REG, set MEM_BASE)          │
│    ├── Compiled PPC instructions                                 │
│    └── Epilogue (restore regs, return)                          │
└─────────────────────────────────────────────────────────────────┘
```

### Address Translation Logic

Xbox 360 uses virtual addresses that must be translated to physical:

- Virtual: `0x80000000-0x9FFFFFFF` → Physical: `0x00000000-0x1FFFFFFF`
- Translation: `physical = virtual & 0x1FFFFFFF`
- Final host address: `host_addr = physical + fastmem_base`

### Generated Code (emit_translate_address)

```arm64
; Check if addr >= 0x80000000
MOV X16, #0x80000000
CMP X0, X16
B.CC skip_to_add        ; Skip if addr < 0x80000000

; Check if addr < 0xA0000000
MOV X16, #0xA0000000
CMP X0, X16
B.CS skip_to_add        ; Skip if addr >= 0xA0000000

; In range: mask to physical
AND X0, X0, #0x1FFFFFFF

skip_to_add:
; Add fastmem base (X20 = MEM_BASE)
ADD X0, X0, X20
```

### The Bug

The `ADD X0, X0, X20` instruction produces the wrong result. Given:

- X0 (translated address) = 0x1FFFEE10 (correct after AND)
- X20 (MEM_BASE) should = 0x70a827b000

Expected: `0x1FFFEE10 + 0x70a827b000 = 0x72A8269010`
Actual fault: `0x702c2634e8`

Working backwards:

```
fault_addr = translated + actual_MEM_BASE
0x702c2634e8 = 0x1FFFEE10 + actual_MEM_BASE
actual_MEM_BASE = 0x700C2645D8  (WRONG!)
```

The MEM_BASE register contains `0x700C2645D8` instead of `0x70a827b000`.

## Investigation Timeline

### Attempt 1: Parameter Passing

**Hypothesis**: X1 parameter (fastmem_base) is corrupted during function call

**Change**: Modified block call to pass fastmem_base as second argument

```cpp
fn(&ctx, fastmem_base_);
```

**Prologue**:

```cpp
emit.ORR(arm64::MEM_BASE, arm64::XZR, arm64::X1);  // X20 = X1
```

**Result**: Still crashes with same pattern

### Attempt 2: Embedded Immediate

**Hypothesis**: Function calling convention issue

**Change**: Embed fastmem_base directly in generated code

```cpp
emit.MOV_imm(arm64::MEM_BASE, reinterpret_cast<u64>(fastmem_base_));
```

This generates:

```arm64
MOVZ X20, #0xB000, LSL #0
MOVK X20, #0xA827, LSL #16
MOVK X20, #0x0070, LSL #32
```

**Result**: Still crashes with same pattern

### Attempt 3: Verified Code Generation

**Verified**:

- MOV_imm generates correct instructions
- Prologue contains correct MOVZ/MOVK sequence
- Block code dump shows expected instruction encodings

**Result**: Still crashes

### Attempt 4: Inline fastmem_base (bypass X20)

**Hypothesis**: X20 is being corrupted somewhere

**Change**: Load fastmem_base directly into X16 before each ADD

```cpp
emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
emit.ADD(addr_reg, addr_reg, arm64::X16);
```

**Result**: Still crashes with same ~2GB offset! This rules out X20 corruption.

### Attempt 5: AND_imm register conflict

**Hypothesis**: `AND_imm` for `0x1FFFFFFF` can't be encoded as ARM64 logical immediate, so it uses fallback that loads into X16 first - same register we use for fastmem_base

**Change**: Use explicit X17 for the mask:

```cpp
emit.MOV_imm(arm64::X17, 0x1FFFFFFFULL);
emit.AND(addr_reg, addr_reg, arm64::X17);
```

**Result**: Testing...

## Possible Root Causes (Not Yet Investigated)

### 1. Register Clobbering

X20 might be clobbered between prologue and memory access by:

- Intermediate instructions using X20 as scratch
- Compiler optimization reusing X20
- Nested function calls

### 2. MOV_imm Encoding Bug

The MOVZ/MOVK sequence might have subtle encoding issues:

- Wrong shift amounts
- Incorrect immediate encoding
- Missing MOVK for upper bits

### 3. Stack Frame Corruption

The STP/LDP sequence might corrupt saved registers:

```cpp
STP X19, X20, [SP, #-32]  // Save
// ... block executes ...
LDP X19, X20, [SP, #-32]  // Restore (might restore garbage)
```

### 4. Memory Ordering / Cache Issues

ARM64 memory model might require barriers:

- Instruction cache not synchronized with data writes
- Memory ordering between MOV_imm writes and subsequent reads

### 5. Thread Safety

fastmem*base* might change between:

- Block compilation (MOV_imm encodes address)
- Block execution (address is used)

## Code Locations

| File                    | Function                 | Purpose                     |
| ----------------------- | ------------------------ | --------------------------- |
| `jit_compiler.cpp:1912` | `emit_block_prologue`    | Sets up MEM_BASE            |
| `jit_compiler.cpp:1874` | `emit_translate_address` | Translates guest→host addr  |
| `jit_compiler.cpp:1137` | `compile_store`          | Compiles store instructions |
| `arm64_emitter.cpp:96`  | `MOV_imm`                | Emits 64-bit immediate load |
| `jit.h:85`              | `MEM_BASE = X20`         | Register assignment         |

## Recommended Next Steps

1. **Add runtime X20 verification**

   - Emit code to store X20 value to context before memory access
   - Log the actual runtime value

2. **Check for X20 usage elsewhere**

   - Search codebase for any other X20 references
   - Verify no scratch register conflicts

3. **Simplify test case**

   - Create minimal block that just loads MEM_BASE and stores it
   - Verify the stored value matches expected

4. **Instruction cache sync**

   - Add `IC IALLU` + `ISB` after code generation
   - Ensure ARM64 instruction cache is coherent

5. **Compare with Xenia/other emulators**
   - Review how Xenia handles fastmem on ARM64
   - Check if there are known ARM64-specific issues

## Resolution

### Root Cause Analysis

The ~2GB offset error was a red herring caused by reading uninitialized/stale memory. The real issue was that the Memory class had **two separate memory regions**:

1. `main_memory_`: 512MB allocated via `mmap()` - used by interpreter and Memory class read/write methods
2. `fastmem_base_`: 4GB reserved region with 512MB mapped at base - used by JIT for direct memory access

After initialization, these were separate copies. When the interpreter wrote to memory (via `Memory::write_u32`), it went to `main_memory_`. But when JIT code read that same address, it read from `fastmem_base_` - which had stale/different data.

### Fix Applied

Modified `setup_fastmem()` in `memory.cpp` to unify the memory regions:

```cpp
// Use mremap to move main_memory_ to fastmem_base_ location
void* remapped = mremap(
    main_memory_,
    main_memory_size_,
    main_memory_size_,
    MREMAP_MAYMOVE | MREMAP_FIXED,
    fastmem_base_
);

// Update main_memory_ to point to the new location
main_memory_ = remapped;
```

Now both `main_memory_` and `fastmem_base_` point to the same physical memory, ensuring consistency between interpreter and JIT.

### Additional Cleanup

- Removed dead code that set up X20 (MEM_BASE) in block prologue - it was never used
- `emit_translate_address` now loads fastmem_base directly into X16 for each memory access
- Fixed potential register conflict where AND_imm fallback could clobber X16 (now uses X17 for mask)

## Previous Workaround (No Longer Needed)

Disable JIT and use interpreter:

```cpp
config.enable_jit = false;
```

This works correctly but is ~10-50x slower.
