/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Integration Test Level 3: First Instructions Execution
 * 
 * Tests:
 * - CPU interpreter working with memory
 * - Instruction fetch and decode
 * - Basic instruction execution
 * - Branch following
 * - Stack setup
 * 
 * Usage: ./test_execute <path_to_xex> [max_instructions]
 */

#include "memory/memory.h"
#include "kernel/xex_loader.h"
#include "cpu/xenon/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <map>

using namespace x360mu;

// Instruction statistics
struct ExecStats {
    u64 total_instructions = 0;
    u64 branches_taken = 0;
    u64 branches_not_taken = 0;
    u64 syscalls = 0;
    u64 unknown_instructions = 0;
    std::map<u8, u64> opcode_counts;  // opcode -> count
    std::set<GuestAddr> visited_addresses;
    GuestAddr min_pc = 0xFFFFFFFFFFFFFFFFULL;
    GuestAddr max_pc = 0;
};

static const char* get_opcode_name(u8 opcode) {
    static const char* names[] = {
        /* 0*/ "?0", "?1", "tdi", "twi", "?4", "?5", "?6", "mulli",
        /* 8*/ "subfic", "?9", "cmpli", "cmpi", "addic", "addic.", "addi", "addis",
        /*16*/ "bc", "sc", "b", "EXT19", "rlwimi", "rlwinm", "?22", "rlwnm",
        /*24*/ "ori", "oris", "xori", "xoris", "andi.", "andis.", "EXT30", "EXT31",
        /*32*/ "lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu",
        /*40*/ "lhz", "lhzu", "lha", "lhau", "sth", "sthu", "lmw", "stmw",
        /*48*/ "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu",
        /*56*/ "?56", "?57", "ld/ldu", "?59", "EXT59", "std/stdu", "?62", "EXT63"
    };
    if (opcode < 64) return names[opcode];
    return "?";
}

static void print_instruction(u64 pc, u32 inst, const DecodedInst& d) {
    printf("  0x%08llX: %08X  %-8s ", 
           (unsigned long long)pc, inst, get_opcode_name(d.opcode));
    
    // Print operands based on opcode type
    switch (d.opcode) {
        case 14: case 15:  // addi, addis
            printf("r%u, r%u, 0x%04X", d.rd, d.ra, (u16)d.simm);
            break;
        case 16:  // bc
            printf("BO=%u, BI=%u, target=+%d", d.bo, d.bi, d.simm * 4);
            break;
        case 18:  // b
            printf("target=0x%08X", (u32)(pc + (d.li << 2)));
            break;
        case 31:  // EXT31
            printf("XO=%u, r%u, r%u, r%u", d.xo, d.rd, d.ra, d.rb);
            break;
        case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:  // load/store
        case 40: case 41: case 42: case 43: case 44: case 45:
            printf("r%u, 0x%X(r%u)", d.rd, (u16)d.simm, d.ra);
            break;
        case 58: case 62:  // ld/std family (DS-form)
            printf("r%u, 0x%X(r%u)", d.rd, (u16)(d.simm & 0xFFFC), d.ra);
            break;
        default:
            printf("r%u, r%u, r%u", d.rd, d.ra, d.rb);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    printf("=============================================\n");
    printf("360μ Integration Test Level 3: Execution\n");
    printf("=============================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <path_to_xex> [max_instructions]\n", argv[0]);
        printf("\nThis test validates:\n");
        printf("  - Instruction fetch from memory\n");
        printf("  - PowerPC decoder integration\n");
        printf("  - Basic interpreter execution\n");
        printf("  - Branch instruction handling\n");
        return 1;
    }
    
    const char* xex_path = argv[1];
    u64 max_instructions = (argc >= 3) ? atoll(argv[2]) : 1000;
    
    printf("XEX Path:         %s\n", xex_path);
    printf("Max Instructions: %llu\n\n", (unsigned long long)max_instructions);
    
    // Initialize Memory
    printf("[INIT] Setting up memory...\n");
    auto memory = std::make_unique<Memory>();
    
    Status status = memory->initialize();
    if (status != Status::Ok) {
        printf("❌ FAIL: Memory initialization failed\n");
        return 1;
    }
    printf("✅ Memory initialized\n");
    
    // Load XEX
    printf("[INIT] Loading XEX...\n");
    XexLoader loader;
    status = loader.load_file(xex_path, memory.get());
    if (status != Status::Ok) {
        printf("❌ FAIL: XEX loading failed\n");
        memory->shutdown();
        return 1;
    }
    
    const XexModule* mod = loader.get_module();
    printf("✅ XEX loaded: %s\n", mod->name.c_str());
    printf("   Entry point: 0x%08X\n\n", (unsigned)mod->entry_point);
    
    // Check entry point has valid code
    u32 first_inst = memory->read_u32(mod->entry_point);
    if (first_inst == 0) {
        printf("⚠️  WARNING: Entry point contains zeros!\n");
        printf("   This usually means the XEX is encrypted/compressed.\n");
        printf("   The loader may need decryption support.\n\n");
        printf("   Attempting to find first non-zero code...\n");
        
        // Scan for first non-zero instruction
        GuestAddr scan_start = mod->base_address;
        GuestAddr scan_end = mod->base_address + mod->image_size;
        GuestAddr found_code = 0;
        
        for (GuestAddr addr = scan_start; addr < scan_end; addr += 4) {
            u32 inst = memory->read_u32(addr);
            if (inst != 0) {
                found_code = addr;
                break;
            }
        }
        
        if (found_code) {
            printf("   Found non-zero code at 0x%08llX\n", (unsigned long long)found_code);
            // Still use the real entry point for now
        } else {
            printf("   No code found in image range!\n");
            printf("❌ FAIL: XEX appears to be entirely encrypted\n");
            memory->shutdown();
            return 1;
        }
    }
    
    // Create interpreter
    printf("[EXEC] Creating interpreter...\n");
    Interpreter interp(memory.get());
    
    // Set up thread context
    ThreadContext ctx;
    ctx.reset();
    ctx.pc = mod->entry_point;
    ctx.running = true;
    
    // Set up initial stack (Xbox 360 stack grows down from 0x70000000)
    ctx.gpr[1] = 0x70000000 - 0x1000;  // Stack pointer
    
    // Initialize some common registers
    ctx.gpr[13] = mod->base_address;  // Small data area (common on PPC)
    
    printf("✅ Context initialized\n");
    printf("   PC = 0x%08llX\n", (unsigned long long)ctx.pc);
    printf("   SP = 0x%08llX\n\n", (unsigned long long)ctx.gpr[1]);
    
    // Execute instructions
    printf("[EXEC] Beginning execution (max %llu instructions)...\n", 
           (unsigned long long)max_instructions);
    printf("=============================================\n");
    
    ExecStats stats;
    u64 last_pc = 0;
    bool stopped_at_syscall = false;
    bool hit_trap = false;
    bool hit_unknown = false;
    
    for (u64 i = 0; i < max_instructions && ctx.running; i++) {
        // Read instruction
        u32 inst = memory->read_u32(ctx.pc);
        
        // Decode
        DecodedInst d = Decoder::decode(inst);
        
        // Track stats
        stats.total_instructions++;
        stats.opcode_counts[d.opcode]++;
        stats.visited_addresses.insert(ctx.pc);
        if (ctx.pc < stats.min_pc) stats.min_pc = ctx.pc;
        if (ctx.pc > stats.max_pc) stats.max_pc = ctx.pc;
        
        // Print first 20 instructions in detail
        if (i < 20) {
            print_instruction(ctx.pc, inst, d);
        } else if (i == 20) {
            printf("  ... (continuing execution, showing summary) ...\n");
        }
        
        // Check for special cases
        if (inst == 0) {
            printf("\n⚠️  Hit zero instruction at 0x%08llX\n", (unsigned long long)ctx.pc);
            break;
        }
        
        // Detect syscall
        if (d.opcode == 17) {  // sc
            stats.syscalls++;
            stopped_at_syscall = true;
            printf("\n🔵 SYSCALL at 0x%08llX (r0=0x%llX, r3=0x%llX)\n", 
                   (unsigned long long)ctx.pc,
                   (unsigned long long)ctx.gpr[0],
                   (unsigned long long)ctx.gpr[3]);
            break;
        }
        
        // Detect trap
        if (d.opcode == 3 || d.opcode == 2) {  // twi, tdi
            hit_trap = true;
            printf("\n⚠️  TRAP at 0x%08llX\n", (unsigned long long)ctx.pc);
            break;
        }
        
        // Detect unknown/invalid
        if (d.type == DecodedInst::Type::Unknown) {
            stats.unknown_instructions++;
            hit_unknown = true;
            printf("\n⚠️  Unknown instruction 0x%08X at 0x%08llX\n", 
                   inst, (unsigned long long)ctx.pc);
            // Don't break - try to continue
        }
        
        // Track branches
        last_pc = ctx.pc;
        
        // Execute
        interp.execute_one(ctx);
        
        // Track taken/not taken branches
        if (d.opcode == 16 || d.opcode == 18 || d.opcode == 19) {  // bc, b, bclr/bcctr
            if (ctx.pc != last_pc + 4) {
                stats.branches_taken++;
            } else {
                stats.branches_not_taken++;
            }
        }
        
        // Detect infinite loop
        if (ctx.pc == last_pc) {
            printf("\n⚠️  Infinite loop detected at 0x%08llX\n", (unsigned long long)ctx.pc);
            break;
        }
    }
    
    printf("=============================================\n\n");
    
    // Print statistics
    printf("[STATS] Execution Statistics\n");
    printf("=============================================\n");
    printf("Total instructions:    %llu\n", (unsigned long long)stats.total_instructions);
    printf("Unique addresses:      %zu\n", stats.visited_addresses.size());
    printf("Address range:         0x%08llX - 0x%08llX\n", 
           (unsigned long long)stats.min_pc, (unsigned long long)stats.max_pc);
    printf("Branches taken:        %llu\n", (unsigned long long)stats.branches_taken);
    printf("Branches not taken:    %llu\n", (unsigned long long)stats.branches_not_taken);
    printf("Syscalls:              %llu\n", (unsigned long long)stats.syscalls);
    printf("Unknown instructions:  %llu\n", (unsigned long long)stats.unknown_instructions);
    printf("\n");
    
    // Top 10 opcodes
    printf("Top 10 Opcodes:\n");
    std::vector<std::pair<u64, u8>> sorted_opcodes;
    for (auto& [opcode, count] : stats.opcode_counts) {
        sorted_opcodes.push_back({count, opcode});
    }
    std::sort(sorted_opcodes.rbegin(), sorted_opcodes.rend());
    
    for (size_t i = 0; i < sorted_opcodes.size() && i < 10; i++) {
        printf("  %-8s (%2u): %llu\n", 
               get_opcode_name(sorted_opcodes[i].second),
               sorted_opcodes[i].second,
               (unsigned long long)sorted_opcodes[i].first);
    }
    printf("\n");
    
    // Final register state
    printf("[STATE] Final Register State\n");
    printf("=============================================\n");
    printf("PC  = 0x%016llX\n", (unsigned long long)ctx.pc);
    printf("LR  = 0x%016llX\n", (unsigned long long)ctx.lr);
    printf("CTR = 0x%016llX\n", (unsigned long long)ctx.ctr);
    printf("CR  = ");
    for (int i = 0; i < 8; i++) {
        printf("%X", ctx.cr[i].to_byte());
    }
    printf("\n");
    printf("XER = SO:%d OV:%d CA:%d\n", ctx.xer.so, ctx.xer.ov, ctx.xer.ca);
    printf("\nGPRs (non-zero):\n");
    for (int i = 0; i < 32; i++) {
        if (ctx.gpr[i] != 0) {
            printf("  r%-2d = 0x%016llX\n", i, (unsigned long long)ctx.gpr[i]);
        }
    }
    printf("\n");
    
    // Cleanup
    memory->shutdown();
    
    // Summary
    printf("=============================================\n");
    printf("SUMMARY: Execution Test\n");
    printf("=============================================\n");
    
    bool passed = (stats.total_instructions >= 10);  // At least 10 instructions
    
    if (passed) {
        printf("✅ Executed %llu instructions successfully\n", 
               (unsigned long long)stats.total_instructions);
        
        if (stopped_at_syscall) {
            printf("🔵 Stopped at syscall (expected - need HLE implementation)\n");
        }
        if (hit_trap) {
            printf("⚠️  Hit trap instruction (may be assertion)\n");
        }
        if (stats.unknown_instructions > 0) {
            printf("⚠️  %llu unknown instructions encountered\n", 
                   (unsigned long long)stats.unknown_instructions);
        }
        
        printf("\n🎉 Level 3 Complete! Basic execution works.\n");
        printf("   The interpreter can fetch, decode, and execute instructions.\n");
        printf("   Next: Run test_syscalls to see what HLE functions are needed.\n");
    } else {
        printf("❌ FAIL: Only executed %llu instructions\n", 
               (unsigned long long)stats.total_instructions);
        printf("   This likely means:\n");
        printf("   - XEX is encrypted and needs decryption\n");
        printf("   - Memory mapping is incorrect\n");
        printf("   - Critical instruction not implemented\n");
    }
    printf("=============================================\n");
    
    return passed ? 0 : 1;
}

