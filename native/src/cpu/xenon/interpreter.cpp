/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC interpreter (fallback execution)
 */

#include "cpu.h"
#include "memory/memory.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-cpu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[CPU] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[CPU ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

Interpreter::Interpreter(Memory* memory) : memory_(memory) {}

u32 Interpreter::execute_one(ThreadContext& ctx) {
    // Fetch instruction (big-endian)
    u32 inst = memory_->read_u32(static_cast<GuestAddr>(ctx.pc));
    
    // Decode
    DecodedInst d = Decoder::decode(inst);
    
    // Execute based on type
    switch (d.type) {
        case DecodedInst::Type::Add:
        case DecodedInst::Type::AddCarrying:
        case DecodedInst::Type::AddExtended:
        case DecodedInst::Type::Sub:
        case DecodedInst::Type::SubCarrying:
        case DecodedInst::Type::SubExtended:
        case DecodedInst::Type::Mul:
        case DecodedInst::Type::MulHigh:
        case DecodedInst::Type::Div:
        case DecodedInst::Type::And:
        case DecodedInst::Type::Or:
        case DecodedInst::Type::Xor:
        case DecodedInst::Type::Nand:
        case DecodedInst::Type::Nor:
        case DecodedInst::Type::Shift:
        case DecodedInst::Type::Rotate:
        case DecodedInst::Type::Compare:
        case DecodedInst::Type::CompareLI:
            exec_integer(ctx, d);
            ctx.pc += 4;
            break;
            
        case DecodedInst::Type::Load:
        case DecodedInst::Type::Store:
        case DecodedInst::Type::LoadUpdate:
        case DecodedInst::Type::StoreUpdate:
        case DecodedInst::Type::LoadMultiple:
        case DecodedInst::Type::StoreMultiple:
            exec_load_store(ctx, d);
            ctx.pc += 4;
            break;
            
        case DecodedInst::Type::Branch:
        case DecodedInst::Type::BranchConditional:
        case DecodedInst::Type::BranchLink:
            exec_branch(ctx, d);
            // PC updated by branch handler
            break;
            
        case DecodedInst::Type::FAdd:
        case DecodedInst::Type::FSub:
        case DecodedInst::Type::FMul:
        case DecodedInst::Type::FDiv:
        case DecodedInst::Type::FMadd:
        case DecodedInst::Type::FNeg:
        case DecodedInst::Type::FAbs:
        case DecodedInst::Type::FCompare:
        case DecodedInst::Type::FConvert:
            // Use complete float handler for opcodes 59/63
            if (d.opcode == 59 || d.opcode == 63) {
                exec_float_complete(ctx, d);
            } else {
                exec_float(ctx, d);
            }
            ctx.pc += 4;
            break;
            
        case DecodedInst::Type::VAdd:
        case DecodedInst::Type::VSub:
        case DecodedInst::Type::VMul:
        case DecodedInst::Type::VDiv:
        case DecodedInst::Type::VPerm:
        case DecodedInst::Type::VMerge:
        case DecodedInst::Type::VSplat:
        case DecodedInst::Type::VCompare:
        case DecodedInst::Type::VLogical:
            exec_vector(ctx, d);
            ctx.pc += 4;
            break;
            
        case DecodedInst::Type::SC:
        case DecodedInst::Type::RFI:
        case DecodedInst::Type::ISYNC:
        case DecodedInst::Type::TW:
        case DecodedInst::Type::TD:
        case DecodedInst::Type::SYNC:
        case DecodedInst::Type::LWSYNC:
        case DecodedInst::Type::EIEIO:
        case DecodedInst::Type::DCBF:
        case DecodedInst::Type::DCBST:
        case DecodedInst::Type::DCBT:
        case DecodedInst::Type::DCBZ:
        case DecodedInst::Type::ICBI:
        case DecodedInst::Type::MTspr:
        case DecodedInst::Type::MFspr:
        case DecodedInst::Type::MTcrf:
        case DecodedInst::Type::MFcr:
        case DecodedInst::Type::CRLogical:
            exec_system(ctx, d);
            ctx.pc += 4;
            break;
            
        default:
            LOGE("Unknown instruction type at 0x%08llX: 0x%08X", ctx.pc, inst);
            ctx.pc += 4;
            break;
    }
    
    // Increment time base register
    // Xbox 360 time base runs at ~50MHz, we approximate with ~4 cycles per instruction
    ctx.time_base += 4;
    
    return 1; // Cycles consumed
}

void Interpreter::execute(ThreadContext& ctx, u64 cycles) {
    // #region agent log - HYPOTHESIS J: Check if interpreter is being used
    static int interp_log = 0;
    if (interp_log++ < 20) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"J\",\"location\":\"interpreter.cpp:execute\",\"message\":\"INTERPRETER FALLBACK\",\"data\":{\"call\":%d,\"pc\":%u,\"cycles\":%llu}}\n", interp_log, (u32)ctx.pc, (unsigned long long)cycles); fclose(f); }
    }
    // #endregion
    
    u64 executed = 0;
    
    // #region agent log - HYPOTHESIS K: Track spin loop addresses
    static GuestAddr last_pc = 0;
    static int same_pc_count = 0;
    // #endregion
    
    while (executed < cycles && ctx.running && !ctx.interrupted) {
        // #region agent log - HYPOTHESIS K: Detect spin loop
        if (ctx.pc == last_pc) {
            same_pc_count++;
            if (same_pc_count == 100 || same_pc_count == 1000) {
                FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
                if (f) { fprintf(f, "{\"hypothesisId\":\"K\",\"location\":\"interpreter.cpp:execute\",\"message\":\"SPIN LOOP DETECTED\",\"data\":{\"pc\":%u,\"count\":%d,\"r3\":%llu,\"r4\":%llu}}\n", (u32)ctx.pc, same_pc_count, ctx.gpr[3], ctx.gpr[4]); fclose(f); }
            }
        } else {
            same_pc_count = 0;
        }
        last_pc = ctx.pc;
        // #endregion
        
        executed += execute_one(ctx);
    }
}

void Interpreter::exec_integer(ThreadContext& ctx, const DecodedInst& d) {
    u64 result = 0;
    u64 ra = (d.ra == 0) ? 0 : ctx.gpr[d.ra];
    u64 rb = ctx.gpr[d.rb];
    
    switch (d.opcode) {
        case 14: // addi
            result = ra + static_cast<s64>(d.simm);
            ctx.gpr[d.rd] = result;
            break;
            
        case 15: // addis
            result = ra + (static_cast<s64>(d.simm) << 16);
            ctx.gpr[d.rd] = result;
            break;
            
        case 8: // subfic
            result = static_cast<u64>(d.simm) - ra;
            ctx.gpr[d.rd] = result;
            // Update CA
            ctx.xer.ca = (static_cast<u64>(d.simm) >= ra);
            break;
            
        case 12: // addic
        case 13: // addic.
            result = ra + static_cast<s64>(d.simm);
            ctx.gpr[d.rd] = result;
            // Update CA (carry out of bit 0)
            ctx.xer.ca = (result < ra);
            if (d.opcode == 13) {
                update_cr0(ctx, static_cast<s64>(result));
            }
            break;
            
        case 7: // mulli
            result = static_cast<s64>(ra) * static_cast<s64>(d.simm);
            ctx.gpr[d.rd] = result;
            break;
            
        case 11: // cmpi
            {
                s64 a = static_cast<s64>(ctx.gpr[d.ra]);
                s64 b = static_cast<s64>(d.simm);
                CRField& cr = ctx.cr[d.crfd];
                cr.lt = a < b;
                cr.gt = a > b;
                cr.eq = a == b;
                cr.so = ctx.xer.so;
            }
            break;
            
        case 10: // cmpli
            {
                u64 a = ctx.gpr[d.ra];
                u64 b = d.uimm;
                CRField& cr = ctx.cr[d.crfd];
                cr.lt = a < b;
                cr.gt = a > b;
                cr.eq = a == b;
                cr.so = ctx.xer.so;
            }
            break;
            
        case 24: // ori
            result = ctx.gpr[d.rs] | d.uimm;
            ctx.gpr[d.ra] = result;
            break;
            
        case 25: // oris
            result = ctx.gpr[d.rs] | (static_cast<u64>(d.uimm) << 16);
            ctx.gpr[d.ra] = result;
            break;
            
        case 26: // xori
            result = ctx.gpr[d.rs] ^ d.uimm;
            ctx.gpr[d.ra] = result;
            break;
            
        case 27: // xoris
            result = ctx.gpr[d.rs] ^ (static_cast<u64>(d.uimm) << 16);
            ctx.gpr[d.ra] = result;
            break;
            
        case 28: // andi.
            result = ctx.gpr[d.rs] & d.uimm;
            ctx.gpr[d.ra] = result;
            update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 29: // andis.
            result = ctx.gpr[d.rs] & (static_cast<u64>(d.uimm) << 16);
            ctx.gpr[d.ra] = result;
            update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 20: // rlwimi
        case 21: // rlwinm
            {
                u32 rs = static_cast<u32>(ctx.gpr[d.rs]);
                u32 sh = d.sh;
                u32 mb = d.mb;
                u32 me = d.me;
                
                // Rotate left
                u32 rotated = (rs << sh) | (rs >> (32 - sh));
                
                // Generate mask
                u32 mask;
                if (mb <= me) {
                    mask = ((1ULL << (me - mb + 1)) - 1) << (31 - me);
                } else {
                    mask = ~(((1ULL << (mb - me - 1)) - 1) << (31 - mb + 1));
                }
                
                if (d.opcode == 20) { // rlwimi
                    result = (rotated & mask) | (ctx.gpr[d.ra] & ~mask);
                } else { // rlwinm
                    result = rotated & mask;
                }
                ctx.gpr[d.ra] = result;
                if (d.rc) {
                    update_cr0(ctx, static_cast<s64>(result));
                }
            }
            break;
            
        case 23: // rlwnm - rotate left word then AND with mask
            {
                u32 rs = static_cast<u32>(ctx.gpr[d.rs]);
                u32 sh = ctx.gpr[d.rb] & 0x1F;
                u32 mb = d.mb;
                u32 me = d.me;
                
                // Rotate left
                u32 rotated = (rs << sh) | (rs >> (32 - sh));
                
                // Generate mask
                u32 mask;
                if (mb <= me) {
                    mask = ((1ULL << (me - mb + 1)) - 1) << (31 - me);
                } else {
                    mask = ~(((1ULL << (mb - me - 1)) - 1) << (31 - mb + 1));
                }
                
                result = rotated & mask;
                ctx.gpr[d.ra] = result;
                if (d.rc) {
                    update_cr0(ctx, static_cast<s64>(result));
                }
            }
            break;
            
        case 30: // 64-bit rotate instructions (rldic, rldicl, rldicr, rldimi, rldcl, rldcr)
            exec_rotate64(ctx, d);
            break;
            
        case 31: // Extended opcodes
            exec_integer_ext31(ctx, d);
            break;
    }
}

// NOTE: exec_integer_ext31 is implemented in interpreter_extended.cpp
// This prevents duplicate symbol errors

#if 0  // Disabled - using extended version instead
void Interpreter::exec_integer_ext31_DISABLED(ThreadContext& ctx, const DecodedInst& d) {
    u64 ra = ctx.gpr[d.ra];
    u64 rb = ctx.gpr[d.rb];
    u64 result = 0;
    
    switch (d.xo) {
        case 266: // add
            result = ra + rb;
            ctx.gpr[d.rd] = result;
            break;
            
        case 10: // addc
            result = ra + rb;
            ctx.xer.ca = (result < ra);
            ctx.gpr[d.rd] = result;
            break;
            
        case 138: // adde
            result = ra + rb + (ctx.xer.ca ? 1 : 0);
            ctx.xer.ca = (result < ra) || (ctx.xer.ca && result == ra);
            ctx.gpr[d.rd] = result;
            break;
            
        case 40: // subf (rb - ra)
            result = rb - ra;
            ctx.gpr[d.rd] = result;
            break;
            
        case 8: // subfc
            result = rb - ra;
            ctx.xer.ca = (rb >= ra);
            ctx.gpr[d.rd] = result;
            break;
            
        case 136: // subfe
            result = ~ra + rb + (ctx.xer.ca ? 1 : 0);
            ctx.xer.ca = (~ra >= rb) || (ctx.xer.ca && ~ra == rb);
            ctx.gpr[d.rd] = result;
            break;
            
        case 104: // neg
            result = ~ra + 1;
            ctx.gpr[d.rd] = result;
            break;
            
        case 235: // mullw
            result = static_cast<s64>(static_cast<s32>(ra)) * static_cast<s64>(static_cast<s32>(rb));
            ctx.gpr[d.rd] = static_cast<u64>(result);
            break;
            
        case 233: // mulld
            result = static_cast<s64>(ra) * static_cast<s64>(rb);
            ctx.gpr[d.rd] = result;
            break;
            
        case 491: // divw
            if (rb != 0) {
                result = static_cast<s32>(ra) / static_cast<s32>(rb);
            }
            ctx.gpr[d.rd] = static_cast<u64>(static_cast<s64>(static_cast<s32>(result)));
            break;
            
        case 459: // divwu
            if (static_cast<u32>(rb) != 0) {
                result = static_cast<u32>(ra) / static_cast<u32>(rb);
            }
            ctx.gpr[d.rd] = result;
            break;
            
        case 28: // and
            result = ra & rb;
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 60: // andc
            result = ra & ~rb;
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 444: // or
            result = ra | rb;
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 412: // orc
            result = ra | ~rb;
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 316: // xor
            result = ra ^ rb;
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 124: // nor
            result = ~(ra | rb);
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 476: // nand
            result = ~(ra & rb);
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 284: // eqv
            result = ~(ra ^ rb);
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 24: // slw
            result = static_cast<u32>(ra) << (rb & 0x1F);
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 536: // srw
            result = static_cast<u32>(ra) >> (rb & 0x1F);
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 792: // sraw
            {
                s32 val = static_cast<s32>(ra);
                u32 shift = rb & 0x3F;
                if (shift > 31) {
                    result = (val < 0) ? 0xFFFFFFFF : 0;
                    ctx.xer.ca = (val < 0);
                } else {
                    result = val >> shift;
                    ctx.xer.ca = (val < 0) && ((val & ((1 << shift) - 1)) != 0);
                }
                ctx.gpr[d.ra] = static_cast<u64>(static_cast<s64>(static_cast<s32>(result)));
                if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            }
            break;
            
        case 824: // srawi
            {
                s32 val = static_cast<s32>(ra);
                u32 shift = d.sh;
                result = val >> shift;
                ctx.xer.ca = (val < 0) && ((val & ((1 << shift) - 1)) != 0);
                ctx.gpr[d.ra] = static_cast<u64>(static_cast<s64>(static_cast<s32>(result)));
                if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            }
            break;
            
        case 26: // cntlzw
            {
                u32 val = static_cast<u32>(ra);
                result = val ? __builtin_clz(val) : 32;
                ctx.gpr[d.ra] = result;
                if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            }
            break;
            
        case 922: // extsh
            result = static_cast<s64>(static_cast<s16>(ra));
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 954: // extsb
            result = static_cast<s64>(static_cast<s8>(ra));
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 986: // extsw
            result = static_cast<s64>(static_cast<s32>(ra));
            ctx.gpr[d.ra] = result;
            if (d.rc) update_cr0(ctx, static_cast<s64>(result));
            break;
            
        case 0: // cmp
            {
                s64 a = static_cast<s64>(ctx.gpr[d.ra]);
                s64 b = static_cast<s64>(ctx.gpr[d.rb]);
                CRField& cr = ctx.cr[d.crfd];
                cr.lt = a < b;
                cr.gt = a > b;
                cr.eq = a == b;
                cr.so = ctx.xer.so;
            }
            break;
            
        case 32: // cmpl
            {
                u64 a = ctx.gpr[d.ra];
                u64 b = ctx.gpr[d.rb];
                CRField& cr = ctx.cr[d.crfd];
                cr.lt = a < b;
                cr.gt = a > b;
                cr.eq = a == b;
                cr.so = ctx.xer.so;
            }
            break;
            
        default:
            LOGE("Unhandled ext31 opcode: %d at 0x%08llX", d.xo, ctx.pc);
            break;
    }
    
    if (d.rc && d.xo != 0 && d.xo != 32) {
        // CR0 update for Rc=1 (except compare which sets CRn directly)
        update_cr0(ctx, static_cast<s64>(ctx.gpr[d.rd]));
    }
}
#endif  // exec_integer_ext31_DISABLED

void Interpreter::exec_load_store(ThreadContext& ctx, const DecodedInst& d) {
    GuestAddr addr;
    
    // Calculate effective address
    if (d.ra == 0 && d.opcode != 31) {
        addr = d.simm;
    } else {
        addr = static_cast<GuestAddr>(ctx.gpr[d.ra] + d.simm);
    }
    
    // Handle indexed addressing for opcode 31
    if (d.opcode == 31) {
        addr = static_cast<GuestAddr>(
            (d.ra == 0 ? 0 : ctx.gpr[d.ra]) + ctx.gpr[d.rb]
        );
    }
    
    switch (d.opcode) {
        case 32: // lwz
            ctx.gpr[d.rd] = read_u32(ctx, addr);
            break;
        case 33: // lwzu
            ctx.gpr[d.rd] = read_u32(ctx, addr);
            ctx.gpr[d.ra] = addr;
            break;
        case 34: // lbz
            ctx.gpr[d.rd] = read_u8(ctx, addr);
            break;
        case 35: // lbzu
            ctx.gpr[d.rd] = read_u8(ctx, addr);
            ctx.gpr[d.ra] = addr;
            break;
        case 40: // lhz
            ctx.gpr[d.rd] = read_u16(ctx, addr);
            break;
        case 41: // lhzu
            ctx.gpr[d.rd] = read_u16(ctx, addr);
            ctx.gpr[d.ra] = addr;
            break;
        case 42: // lha
            ctx.gpr[d.rd] = static_cast<s64>(static_cast<s16>(read_u16(ctx, addr)));
            break;
        case 43: // lhau
            ctx.gpr[d.rd] = static_cast<s64>(static_cast<s16>(read_u16(ctx, addr)));
            ctx.gpr[d.ra] = addr;
            break;
        case 36: // stw
            write_u32(ctx, addr, static_cast<u32>(ctx.gpr[d.rs]));
            break;
        case 37: // stwu
            write_u32(ctx, addr, static_cast<u32>(ctx.gpr[d.rs]));
            ctx.gpr[d.ra] = addr;
            break;
        case 38: // stb
            write_u8(ctx, addr, static_cast<u8>(ctx.gpr[d.rs]));
            break;
        case 39: // stbu
            write_u8(ctx, addr, static_cast<u8>(ctx.gpr[d.rs]));
            ctx.gpr[d.ra] = addr;
            break;
        case 44: // sth
            write_u16(ctx, addr, static_cast<u16>(ctx.gpr[d.rs]));
            break;
        case 45: // sthu
            write_u16(ctx, addr, static_cast<u16>(ctx.gpr[d.rs]));
            ctx.gpr[d.ra] = addr;
            break;
        case 46: // lmw
            for (u32 r = d.rd; r < 32; r++) {
                ctx.gpr[r] = read_u32(ctx, addr);
                addr += 4;
            }
            break;
        case 47: // stmw
            for (u32 r = d.rs; r < 32; r++) {
                write_u32(ctx, addr, static_cast<u32>(ctx.gpr[r]));
                addr += 4;
            }
            break;
        case 48: // lfs - Load Floating-Point Single
            {
                u32 fval = read_u32(ctx, addr);
                float f = *reinterpret_cast<float*>(&fval);
                ctx.fpr[d.rd] = static_cast<f64>(f);
            }
            break;
        case 49: // lfsu - Load Floating-Point Single with Update
            {
                u32 fval = read_u32(ctx, addr);
                float f = *reinterpret_cast<float*>(&fval);
                ctx.fpr[d.rd] = static_cast<f64>(f);
                ctx.gpr[d.ra] = addr;
            }
            break;
        case 50: // lfd - Load Floating-Point Double
            {
                u64 dval = read_u64(ctx, addr);
                ctx.fpr[d.rd] = *reinterpret_cast<f64*>(&dval);
            }
            break;
        case 51: // lfdu - Load Floating-Point Double with Update
            {
                u64 dval = read_u64(ctx, addr);
                ctx.fpr[d.rd] = *reinterpret_cast<f64*>(&dval);
                ctx.gpr[d.ra] = addr;
            }
            break;
        case 52: // stfs - Store Floating-Point Single
            {
                float f = static_cast<float>(ctx.fpr[d.rs]);
                u32 fval = *reinterpret_cast<u32*>(&f);
                write_u32(ctx, addr, fval);
            }
            break;
        case 53: // stfsu - Store Floating-Point Single with Update
            {
                float f = static_cast<float>(ctx.fpr[d.rs]);
                u32 fval = *reinterpret_cast<u32*>(&f);
                write_u32(ctx, addr, fval);
                ctx.gpr[d.ra] = addr;
            }
            break;
        case 54: // stfd - Store Floating-Point Double
            {
                u64 dval = *reinterpret_cast<u64*>(&ctx.fpr[d.rs]);
                write_u64(ctx, addr, dval);
            }
            break;
        case 55: // stfdu - Store Floating-Point Double with Update
            {
                u64 dval = *reinterpret_cast<u64*>(&ctx.fpr[d.rs]);
                write_u64(ctx, addr, dval);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 58: // ld/ldu/lwa (DS-form)
        case 62: // std/stdu (DS-form)
            exec_load_store_ds(ctx, d);
            return; // exec_load_store_ds handles everything
            
        case 31: // Extended indexed load/store (lwzx, lbzx, stwx, lwarx, stwcx, etc.)
            exec_integer_ext31(ctx, d);
            return;
            
        default:
            LOGE("Unhandled load/store opcode: %d", d.opcode);
            break;
    }
}

void Interpreter::exec_branch(ThreadContext& ctx, const DecodedInst& d) {
    bool take_branch = false;
    u64 target = 0;
    
    switch (d.opcode) {
        case 18: // b, ba, bl, bla
            take_branch = true;
            if (d.raw & 2) { // AA - absolute
                target = d.li;
            } else {
                target = ctx.pc + d.li;
            }
            if (d.raw & 1) { // LK - link
                ctx.lr = ctx.pc + 4;
            }
            break;
            
        case 16: // bc, bca, bcl, bcla
            {
                bool ctr_ok = true;
                bool cond_ok = true;
                
                // Decrement CTR if BO[2] = 0
                if (!(d.bo & 0x04)) {
                    ctx.ctr--;
                    ctr_ok = (d.bo & 0x02) ? (ctx.ctr == 0) : (ctx.ctr != 0);
                }
                
                // Check condition if BO[0] = 0
                if (!(d.bo & 0x10)) {
                    bool cond = (ctx.cr[d.bi / 4].to_byte() >> (3 - (d.bi % 4))) & 1;
                    cond_ok = (d.bo & 0x08) ? cond : !cond;
                }
                
                take_branch = ctr_ok && cond_ok;
                
                if (take_branch) {
                    if (d.raw & 2) { // AA
                        target = d.simm;
                    } else {
                        target = ctx.pc + d.simm;
                    }
                }
                
                if (d.raw & 1) { // LK
                    ctx.lr = ctx.pc + 4;
                }
            }
            break;
            
        case 19: // bclr, bcctr
            {
                bool ctr_ok = true;
                bool cond_ok = true;
                
                if (d.xo == 16) { // bclr
                    // Decrement CTR if BO[2] = 0
                    if (!(d.bo & 0x04)) {
                        ctx.ctr--;
                        ctr_ok = (d.bo & 0x02) ? (ctx.ctr == 0) : (ctx.ctr != 0);
                    }
                    target = ctx.lr & ~3ULL;
                } else if (d.xo == 528) { // bcctr
                    target = ctx.ctr & ~3ULL;
                }
                
                // Check condition if BO[0] = 0
                if (!(d.bo & 0x10)) {
                    bool cond = (ctx.cr[d.bi / 4].to_byte() >> (3 - (d.bi % 4))) & 1;
                    cond_ok = (d.bo & 0x08) ? cond : !cond;
                }
                
                take_branch = ctr_ok && cond_ok;
                
                if (d.raw & 1) { // LK
                    ctx.lr = ctx.pc + 4;
                }
            }
            break;
    }
    
    if (take_branch) {
        ctx.pc = target;
    } else {
        ctx.pc += 4;
    }
}

void Interpreter::exec_float(ThreadContext& ctx, const DecodedInst& d) {
    // Simplified float handling
    f64 fra = ctx.fpr[d.ra];
    f64 frb = ctx.fpr[d.rb];
    f64 frc = ctx.fpr[(d.raw >> 6) & 0x1F]; // FRC field
    f64 result = 0.0;
    
    switch (d.xo) {
        case 21: // fadd
            result = fra + frb;
            break;
        case 20: // fsub
            result = fra - frb;
            break;
        case 25: // fmul
            result = fra * frc;
            break;
        case 18: // fdiv
            result = fra / frb;
            break;
        case 29: // fmadd
            result = fra * frc + frb;
            break;
        case 28: // fmsub
            result = fra * frc - frb;
            break;
        default:
            LOGE("Unhandled float opcode: %d", d.xo);
            break;
    }
    
    ctx.fpr[d.rd] = result;
    
    if (d.rc) {
        update_cr1(ctx);
    }
}

void Interpreter::exec_vector(ThreadContext& ctx, const DecodedInst& d) {
    // VMX128 instructions - complex, implement key ones
    LOGD("Vector instruction at 0x%08llX (not fully implemented)", ctx.pc);
}

void Interpreter::exec_system(ThreadContext& ctx, const DecodedInst& d) {
    switch (d.type) {
        case DecodedInst::Type::SC:
            // System call - trigger HLE handler
            ctx.interrupted = true;
            break;
            
        case DecodedInst::Type::MTspr:
            {
                u32 spr = ((d.raw >> 16) & 0x1F) | ((d.raw >> 6) & 0x3E0);
                switch (spr) {
                    case 8: // LR
                        ctx.lr = ctx.gpr[d.rs];
                        break;
                    case 9: // CTR
                        ctx.ctr = ctx.gpr[d.rs];
                        break;
                    case 1: // XER
                        ctx.xer.from_u32(static_cast<u32>(ctx.gpr[d.rs]));
                        break;
                    default:
                        LOGD("mtspr SPR%d = 0x%016llX", spr, ctx.gpr[d.rs]);
                        break;
                }
            }
            break;
            
        case DecodedInst::Type::MFspr:
            {
                u32 spr = ((d.raw >> 16) & 0x1F) | ((d.raw >> 6) & 0x3E0);
                switch (spr) {
                    case 8: // LR
                        ctx.gpr[d.rd] = ctx.lr;
                        break;
                    case 9: // CTR
                        ctx.gpr[d.rd] = ctx.ctr;
                        break;
                    case 1: // XER
                        ctx.gpr[d.rd] = ctx.xer.to_u32();
                        break;
                    case 268: // TBL (Time Base Lower)
                    case 284: // TBL alternate encoding
                        // Return lower 32 bits of time base
                        ctx.gpr[d.rd] = static_cast<u32>(ctx.time_base);
                        break;
                    case 269: // TBU (Time Base Upper)
                    case 285: // TBU alternate encoding
                        // Return upper 32 bits of time base
                        ctx.gpr[d.rd] = static_cast<u32>(ctx.time_base >> 32);
                        break;
                    default:
                        LOGD("mfspr r%d = SPR%d", d.rd, spr);
                        ctx.gpr[d.rd] = 0;
                        break;
                }
            }
            break;
            
        case DecodedInst::Type::MFcr:
            {
                u32 cr = 0;
                for (int i = 0; i < 8; i++) {
                    cr |= ctx.cr[i].to_byte() << (28 - i * 4);
                }
                ctx.gpr[d.rd] = cr;
            }
            break;
            
        case DecodedInst::Type::MTcrf:
            {
                u32 mask = (d.raw >> 12) & 0xFF;
                u32 cr = static_cast<u32>(ctx.gpr[d.rs]);
                for (int i = 0; i < 8; i++) {
                    if (mask & (0x80 >> i)) {
                        ctx.cr[i].from_byte((cr >> (28 - i * 4)) & 0xF);
                    }
                }
            }
            break;
            
        case DecodedInst::Type::SYNC:
            // Full memory barrier - ensures all preceding memory operations complete
            // before any subsequent memory operations
            std::atomic_thread_fence(std::memory_order_seq_cst);
            break;
            
        case DecodedInst::Type::LWSYNC:
            // Lightweight sync - load-store ordering (acquire-release semantics)
            // Ensures loads before lwsync complete before stores after lwsync
            std::atomic_thread_fence(std::memory_order_acq_rel);
            break;
            
        case DecodedInst::Type::EIEIO:
            // Enforce In-Order Execution of I/O
            // For non-cacheable memory (MMIO), ensures I/O operations are ordered
            // Acts as a release barrier for stores
            std::atomic_thread_fence(std::memory_order_release);
            break;
            
        case DecodedInst::Type::ISYNC:
            // Instruction synchronize - context synchronizing instruction
            // Ensures all previous instructions have completed (including branches)
            // before instruction fetch resumes
            // For JIT: would need to flush instruction cache
            std::atomic_thread_fence(std::memory_order_seq_cst);
            break;
            
        case DecodedInst::Type::DCBF:
        case DecodedInst::Type::DCBST:
        case DecodedInst::Type::DCBT:
        case DecodedInst::Type::DCBZ:
        case DecodedInst::Type::ICBI:
            // Cache operations - mostly no-op
            if (d.type == DecodedInst::Type::DCBZ) {
                // Zero a cache line
                GuestAddr addr = static_cast<GuestAddr>(
                    (d.ra == 0 ? 0 : ctx.gpr[d.ra]) + ctx.gpr[d.rb]
                );
                addr &= ~31; // Align to 32 bytes
                memory_->zero_bytes(addr, 32);
            }
            break;
            
        case DecodedInst::Type::TW:
        case DecodedInst::Type::TD:
            // Trap instructions - check condition and trigger exception if matched
            {
                // For now, just ignore traps (don't trigger exception)
                // In a full implementation, we'd check the TO field and operands
                LOGD("Trap instruction at 0x%08llX (ignored)", ctx.pc);
            }
            break;
            
        case DecodedInst::Type::CRLogical:
            // CR logical operations (opcode 19)
            {
                u32 xo = (d.raw >> 1) & 0x3FF;
                u32 crbD = (d.raw >> 21) & 0x1F;  // Target CR bit
                u32 crbA = (d.raw >> 16) & 0x1F;  // Source CR bit A
                u32 crbB = (d.raw >> 11) & 0x1F;  // Source CR bit B
                
                // Get CR bits (CR is stored as array of 4-bit fields)
                auto get_crbit = [&](u32 bit) -> bool {
                    u32 field = bit / 4;
                    u32 pos = 3 - (bit % 4);
                    return (ctx.cr[field].to_byte() >> pos) & 1;
                };
                
                auto set_crbit = [&](u32 bit, bool val) {
                    u32 field = bit / 4;
                    u32 pos = 3 - (bit % 4);
                    u8 byte = ctx.cr[field].to_byte();
                    if (val) {
                        byte |= (1 << pos);
                    } else {
                        byte &= ~(1 << pos);
                    }
                    ctx.cr[field].from_byte(byte);
                };
                
                bool a = get_crbit(crbA);
                bool b = get_crbit(crbB);
                bool result = false;
                
                switch (xo) {
                    case 257: // crand
                        result = a && b;
                        break;
                    case 449: // cror
                        result = a || b;
                        break;
                    case 225: // crnand
                        result = !(a && b);
                        break;
                    case 33: // crnor
                        result = !(a || b);
                        break;
                    case 193: // crxor
                        result = a != b;
                        break;
                    case 289: // creqv
                        result = a == b;
                        break;
                    case 129: // crandc
                        result = a && !b;
                        break;
                    case 417: // crorc
                        result = a || !b;
                        break;
                    case 0: // mcrf - Move CR field
                        {
                            u32 crfD = (d.raw >> 23) & 0x7;
                            u32 crfS = (d.raw >> 18) & 0x7;
                            ctx.cr[crfD] = ctx.cr[crfS];
                        }
                        return; // mcrf doesn't use crbD result
                    default:
                        LOGD("Unknown CR logical xo=%d at 0x%08llX", xo, ctx.pc);
                        return;
                }
                
                set_crbit(crbD, result);
            }
            break;
            
        case DecodedInst::Type::RFI:
            // Return from interrupt - would restore MSR and jump to SRR0
            // For HLE, we don't really use this
            LOGD("RFI at 0x%08llX (ignored)", ctx.pc);
            break;
            
        default:
            LOGE("Unhandled system instruction type at 0x%08llX", ctx.pc);
            break;
    }
}

// Memory access helpers
u8 Interpreter::read_u8(ThreadContext& ctx, GuestAddr addr) {
    return memory_->read_u8(addr);
}

u16 Interpreter::read_u16(ThreadContext& ctx, GuestAddr addr) {
    return memory_->read_u16(addr);
}

u32 Interpreter::read_u32(ThreadContext& ctx, GuestAddr addr) {
    return memory_->read_u32(addr);
}

u64 Interpreter::read_u64(ThreadContext& ctx, GuestAddr addr) {
    return memory_->read_u64(addr);
}

void Interpreter::write_u8(ThreadContext& ctx, GuestAddr addr, u8 value) {
    memory_->write_u8(addr, value);
}

void Interpreter::write_u16(ThreadContext& ctx, GuestAddr addr, u16 value) {
    memory_->write_u16(addr, value);
}

void Interpreter::write_u32(ThreadContext& ctx, GuestAddr addr, u32 value) {
    memory_->write_u32(addr, value);
}

void Interpreter::write_u64(ThreadContext& ctx, GuestAddr addr, u64 value) {
    memory_->write_u64(addr, value);
}

// CR update helpers
void Interpreter::update_cr0(ThreadContext& ctx, s64 result) {
    ctx.cr[0].lt = result < 0;
    ctx.cr[0].gt = result > 0;
    ctx.cr[0].eq = result == 0;
    ctx.cr[0].so = ctx.xer.so;
}

void Interpreter::update_cr1(ThreadContext& ctx) {
    // CR1 reflects FPSCR[0:3]
    ctx.cr[1].from_byte((ctx.fpscr >> 28) & 0xF);
}

} // namespace x360mu

