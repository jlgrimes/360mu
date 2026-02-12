/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Extended PowerPC interpreter instructions
 * 
 * This file adds the remaining instructions needed for real game compatibility:
 * - 64-bit integer operations
 * - Complete floating-point operations  
 * - Atomic (load-reserved/store-conditional)
 * - Extended load/store (indexed)
 * - String operations
 * - Full VMX128 vector unit
 */

#include "cpu.h"
#include "../../memory/memory.h"
#include <cmath>
#include <cstring>
#include <atomic>

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
#define LOGD(...)
#endif

namespace x360mu {

// Helper: Sign extend
inline s64 sign_extend(u64 val, int bits) {
    u64 mask = 1ULL << (bits - 1);
    return static_cast<s64>((val ^ mask) - mask);
}

// Helper: Rotate left 64-bit
inline u64 rotl64(u64 val, int shift) {
    shift &= 63;
    return (val << shift) | (val >> (64 - shift));
}

// Helper: Rotate left 32-bit
inline u32 rotl32(u32 val, int shift) {
    shift &= 31;
    return (val << shift) | (val >> (32 - shift));
}

// Helper: Generate mask for rotate instructions
inline u64 mask64(int mb, int me) {
    u64 mask;
    if (mb <= me) {
        mask = ((1ULL << (me - mb + 1)) - 1) << (63 - me);
    } else {
        mask = ~(((1ULL << (mb - me - 1)) - 1) << (63 - mb + 1));
    }
    return mask;
}

//=============================================================================
// Integer Extended - 64-bit operations (opcode 31 and 30)
//=============================================================================

void Interpreter::exec_integer_ext31(ThreadContext& ctx, const DecodedInst& d) {
    u64 ra = ctx.gpr[d.ra];
    u64 rb = ctx.gpr[d.rb];
    u64 result = 0;
    
    switch (d.xo) {
        // --- Addition family ---
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
            
        case 202: // addze
            result = ra + (ctx.xer.ca ? 1 : 0);
            ctx.xer.ca = (result < ra);
            ctx.gpr[d.rd] = result;
            break;
            
        case 234: // addme
            result = ra + ctx.xer.ca - 1;
            ctx.xer.ca = (ra != 0) || ctx.xer.ca;
            ctx.gpr[d.rd] = result;
            break;
            
        // --- Subtraction family ---
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
            
        case 200: // subfze
            result = ~ra + (ctx.xer.ca ? 1 : 0);
            ctx.xer.ca = (ra == 0) || ctx.xer.ca;
            ctx.gpr[d.rd] = result;
            break;
            
        case 232: // subfme
            result = ~ra + ctx.xer.ca - 1;
            ctx.xer.ca = (ra != 0xFFFFFFFFFFFFFFFFULL) || ctx.xer.ca;
            ctx.gpr[d.rd] = result;
            break;
            
        case 104: // neg
            result = ~ra + 1;
            ctx.gpr[d.rd] = result;
            break;
            
        // --- Multiplication ---
        case 235: // mullw
            result = static_cast<s64>(static_cast<s32>(ra)) * 
                     static_cast<s64>(static_cast<s32>(rb));
            ctx.gpr[d.rd] = static_cast<u64>(result);
            break;
            
        case 233: // mulld
            result = static_cast<s64>(ra) * static_cast<s64>(rb);
            ctx.gpr[d.rd] = result;
            break;
            
        case 75: // mulhw
            {
                s64 prod = static_cast<s64>(static_cast<s32>(ra)) * 
                           static_cast<s64>(static_cast<s32>(rb));
                result = static_cast<u64>(prod >> 32);
                ctx.gpr[d.rd] = result;
            }
            break;
            
        case 11: // mulhwu
            {
                u64 prod = static_cast<u64>(static_cast<u32>(ra)) * 
                           static_cast<u64>(static_cast<u32>(rb));
                result = prod >> 32;
                ctx.gpr[d.rd] = result;
            }
            break;
            
        case 73: // mulhd
            {
                // 64x64 -> 128, take high 64 bits
                __int128 prod = static_cast<__int128>(static_cast<s64>(ra)) * 
                                static_cast<__int128>(static_cast<s64>(rb));
                result = static_cast<u64>(prod >> 64);
                ctx.gpr[d.rd] = result;
            }
            break;
            
        case 9: // mulhdu
            {
                __uint128_t prod = static_cast<__uint128_t>(ra) * 
                                   static_cast<__uint128_t>(rb);
                result = static_cast<u64>(prod >> 64);
                ctx.gpr[d.rd] = result;
            }
            break;
            
        // --- Division ---
        case 491: // divw
            if (static_cast<s32>(rb) != 0) {
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
            
        case 489: // divd
            if (static_cast<s64>(rb) != 0) {
                result = static_cast<s64>(ra) / static_cast<s64>(rb);
            }
            ctx.gpr[d.rd] = result;
            break;
            
        case 457: // divdu
            if (rb != 0) {
                result = ra / rb;
            }
            ctx.gpr[d.rd] = result;
            break;
            
        // --- Logical (X-form: RS=d.rd is source, RA=d.ra is destination) ---
        case 28: // and
            result = ctx.gpr[d.rd] & rb;
            ctx.gpr[d.ra] = result;
            break;

        case 60: // andc
            result = ctx.gpr[d.rd] & ~rb;
            ctx.gpr[d.ra] = result;
            break;

        case 444: // or
            result = ctx.gpr[d.rd] | rb;
            ctx.gpr[d.ra] = result;
            break;

        case 412: // orc
            result = ctx.gpr[d.rd] | ~rb;
            ctx.gpr[d.ra] = result;
            break;

        case 316: // xor
            result = ctx.gpr[d.rd] ^ rb;
            ctx.gpr[d.ra] = result;
            break;

        case 124: // nor
            result = ~(ctx.gpr[d.rd] | rb);
            ctx.gpr[d.ra] = result;
            break;

        case 476: // nand
            result = ~(ctx.gpr[d.rd] & rb);
            ctx.gpr[d.ra] = result;
            break;

        case 284: // eqv
            result = ~(ctx.gpr[d.rd] ^ rb);
            ctx.gpr[d.ra] = result;
            break;
            
        // --- Shifts (32-bit) ---
        case 24: // slw
            {
                u32 shift = rb & 0x3F;
                if (shift < 32) {
                    result = (static_cast<u32>(ra) << shift);
                } else {
                    result = 0;
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 536: // srw
            {
                u32 shift = rb & 0x3F;
                if (shift < 32) {
                    result = (static_cast<u32>(ra) >> shift);
                } else {
                    result = 0;
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 792: // sraw
            {
                s32 val = static_cast<s32>(ra);
                u32 shift = rb & 0x3F;
                if (shift == 0) {
                    result = val;
                    ctx.xer.ca = false;
                } else if (shift < 32) {
                    result = val >> shift;
                    ctx.xer.ca = (val < 0) && ((val & ((1 << shift) - 1)) != 0);
                } else {
                    result = (val < 0) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                    ctx.xer.ca = (val < 0);
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 824: // srawi
            {
                s32 val = static_cast<s32>(ra);
                u32 shift = d.sh;
                result = val >> shift;
                ctx.xer.ca = (val < 0) && ((val & ((1 << shift) - 1)) != 0);
                ctx.gpr[d.ra] = static_cast<u64>(static_cast<s64>(static_cast<s32>(result)));
            }
            break;
            
        // --- Shifts (64-bit) ---
        case 27: // sld - X-form: RS is at d.rd, RA is destination
            {
                u64 rs = ctx.gpr[d.rd];  // Source is RS (d.rd position)
                u32 shift = rb & 0x7F;
                if (shift < 64) {
                    result = rs << shift;
                } else {
                    result = 0;
                }
                ctx.gpr[d.ra] = result;  // Destination is RA (d.ra position)
            }
            break;
            
        case 539: // srd - X-form: RS is at d.rd, RA is destination
            {
                u64 rs = ctx.gpr[d.rd];  // Source is RS (d.rd position)
                u32 shift = rb & 0x7F;
                if (shift < 64) {
                    result = rs >> shift;
                } else {
                    result = 0;
                }
                ctx.gpr[d.ra] = result;  // Destination is RA (d.ra position)
            }
            break;
            
        case 794: // srad - X-form: RS is at d.rd, RA is destination
            {
                s64 val = static_cast<s64>(ctx.gpr[d.rd]);  // Source is RS (d.rd position)
                u32 shift = rb & 0x7F;
                if (shift == 0) {
                    result = val;
                    ctx.xer.ca = false;
                } else if (shift < 64) {
                    result = val >> shift;
                    ctx.xer.ca = (val < 0) && ((val & ((1ULL << shift) - 1)) != 0);
                } else {
                    result = (val < 0) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                    ctx.xer.ca = (val < 0);
                }
                ctx.gpr[d.ra] = result;  // Destination is RA (d.ra position)
            }
            break;
            
        case 826: // sradi
        case 827: // sradi (sh[5] = 1)
            {
                u64 rs = ctx.gpr[d.rd];  // Source register is RS (bits 6-10)
                s64 val = static_cast<s64>(rs);
                // 6-bit shift: sh[0:4] in bits 16-20, sh[5] in bit 30 (part of xo)
                u32 shift = ((d.raw >> 11) & 0x1F) | (((d.raw >> 1) & 1) << 5);
                if (shift == 0) {
                    result = val;
                    ctx.xer.ca = false;
                } else {
                    result = val >> shift;
                    ctx.xer.ca = (val < 0) && ((val & ((1ULL << shift) - 1)) != 0);
                }
                ctx.gpr[d.ra] = result;  // Destination is RA (bits 11-15)
            }
            break;
            
        case 413: // sradi (another form - XO=413 for extended instructions)
            {
                u64 rs = ctx.gpr[d.rd];
                s64 val = static_cast<s64>(rs);
                u32 shift = ((d.raw >> 11) & 0x1F) | (((d.raw >> 1) & 1) << 5);
                if (shift == 0) {
                    result = val;
                    ctx.xer.ca = false;
                } else {
                    result = val >> shift;
                    ctx.xer.ca = (val < 0) && ((val & ((1ULL << shift) - 1)) != 0);
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        // --- Count leading zeros ---
        case 26: // cntlzw
            {
                u32 val = static_cast<u32>(ra);
                result = val ? __builtin_clz(val) : 32;
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 58: // cntlzd
            {
                result = ra ? __builtin_clzll(ra) : 64;
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 122: // popcntb - Population Count Bytes
            {
                // Count bits set in each byte of the source register
                u64 rs = ctx.gpr[d.rd];  // Source register
                result = 0;
                for (int i = 0; i < 8; i++) {
                    u8 byte = (rs >> (i * 8)) & 0xFF;
                    u8 count = __builtin_popcount(byte);
                    result |= static_cast<u64>(count) << (i * 8);
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 378: // popcntw - Population Count Word
            {
                u64 rs = ctx.gpr[d.rd];
                // Count bits in lower and upper 32-bit words separately
                u32 lo = __builtin_popcount(static_cast<u32>(rs));
                u32 hi = __builtin_popcount(static_cast<u32>(rs >> 32));
                result = (static_cast<u64>(hi) << 32) | lo;
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 506: // popcntd - Population Count Doubleword
            {
                u64 rs = ctx.gpr[d.rd];
                result = __builtin_popcountll(rs);
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 508: // cmpb - Compare Bytes
            {
                // For each byte position, if bytes match set result byte to 0xFF, else 0x00
                u64 rs = ctx.gpr[d.rd];
                result = 0;
                for (int i = 0; i < 8; i++) {
                    u8 byte_a = (rs >> (i * 8)) & 0xFF;
                    u8 byte_b = (rb >> (i * 8)) & 0xFF;
                    if (byte_a == byte_b) {
                        result |= 0xFFULL << (i * 8);
                    }
                }
                ctx.gpr[d.ra] = result;
            }
            break;
            
        case 954 + 64: // prtyw - Parity Word (XO=1018, but overlaps, use different)
            // Note: prtyw is XO=154
            break;
            
        // --- Sign extension ---
        case 922: // extsh
            result = static_cast<s64>(static_cast<s16>(ra));
            ctx.gpr[d.ra] = result;
            break;
            
        case 954: // extsb
            result = static_cast<s64>(static_cast<s8>(ra));
            ctx.gpr[d.ra] = result;
            break;
            
        case 986: // extsw
            result = static_cast<s64>(static_cast<s32>(ra));
            ctx.gpr[d.ra] = result;
            break;
            
        // --- Compare ---
        case 0: // cmp (signed)
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
            
        case 32: // cmpl (unsigned)
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
            
        // --- Indexed load/store ---
        case 23: // lwzx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u32(ctx, addr);
            }
            break;
            
        case 55: // lwzux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u32(ctx, addr);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 87: // lbzx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u8(ctx, addr);
            }
            break;
            
        case 119: // lbzux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u8(ctx, addr);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 279: // lhzx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u16(ctx, addr);
            }
            break;
            
        case 311: // lhzux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u16(ctx, addr);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 343: // lhax
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = static_cast<s64>(static_cast<s16>(read_u16(ctx, addr)));
            }
            break;
            
        case 375: // lhaux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = static_cast<s64>(static_cast<s16>(read_u16(ctx, addr)));
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 341: // lwax - Load Word Algebraic Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = static_cast<s64>(static_cast<s32>(read_u32(ctx, addr)));
            }
            break;
            
        case 373: // lwaux - Load Word Algebraic with Update Indexed
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = static_cast<s64>(static_cast<s32>(read_u32(ctx, addr)));
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 21: // ldx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u64(ctx, addr);
            }
            break;
            
        case 53: // ldux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u64(ctx, addr);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 151: // stwx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                write_u32(ctx, addr, static_cast<u32>(ctx.gpr[d.rs]));
            }
            break;
            
        case 183: // stwux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                write_u32(ctx, addr, static_cast<u32>(ctx.gpr[d.rs]));
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 215: // stbx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                write_u8(ctx, addr, static_cast<u8>(ctx.gpr[d.rs]));
            }
            break;
            
        case 247: // stbux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                write_u8(ctx, addr, static_cast<u8>(ctx.gpr[d.rs]));
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 407: // sthx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                write_u16(ctx, addr, static_cast<u16>(ctx.gpr[d.rs]));
            }
            break;
            
        case 439: // sthux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                write_u16(ctx, addr, static_cast<u16>(ctx.gpr[d.rs]));
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 149: // stdx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                write_u64(ctx, addr, ctx.gpr[d.rs]);
            }
            break;
            
        case 181: // stdux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                write_u64(ctx, addr, ctx.gpr[d.rs]);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        // --- Byte reverse load/store ---
        case 534: // lwbrx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u32 val = read_u32(ctx, addr);
                // Byte reverse
                val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
                      ((val << 8) & 0xFF0000) | ((val << 24) & 0xFF000000);
                ctx.gpr[d.rd] = val;
            }
            break;
            
        case 790: // lhbrx - Load Halfword Byte-Reverse Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u16 val = read_u16(ctx, addr);
                val = ((val >> 8) & 0xFF) | ((val << 8) & 0xFF00);
                ctx.gpr[d.rd] = val;
            }
            break;
            
        case 532: // ldbrx - Load Doubleword Byte-Reverse Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u64 val = read_u64(ctx, addr);
                // Byte reverse 64-bit value
                val = ((val >> 56) & 0xFF) |
                      ((val >> 40) & 0xFF00) |
                      ((val >> 24) & 0xFF0000) |
                      ((val >> 8)  & 0xFF000000ULL) |
                      ((val << 8)  & 0xFF00000000ULL) |
                      ((val << 24) & 0xFF0000000000ULL) |
                      ((val << 40) & 0xFF000000000000ULL) |
                      ((val << 56) & 0xFF00000000000000ULL);
                ctx.gpr[d.rd] = val;
            }
            break;
            
        case 662: // stwbrx - Store Word Byte-Reverse Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u32 val = static_cast<u32>(ctx.gpr[d.rs]);
                // Byte reverse
                val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
                      ((val << 8) & 0xFF0000) | ((val << 24) & 0xFF000000);
                write_u32(ctx, addr, val);
            }
            break;
            
        case 918: // sthbrx - Store Halfword Byte-Reverse Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u16 val = static_cast<u16>(ctx.gpr[d.rs]);
                val = ((val >> 8) & 0xFF) | ((val << 8) & 0xFF00);
                write_u16(ctx, addr, val);
            }
            break;
            
        case 660: // stdbrx - Store Doubleword Byte-Reverse Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u64 val = ctx.gpr[d.rs];
                // Byte reverse 64-bit value
                val = ((val >> 56) & 0xFF) |
                      ((val >> 40) & 0xFF00) |
                      ((val >> 24) & 0xFF0000) |
                      ((val >> 8)  & 0xFF000000ULL) |
                      ((val << 8)  & 0xFF00000000ULL) |
                      ((val << 24) & 0xFF0000000000ULL) |
                      ((val << 40) & 0xFF000000000000ULL) |
                      ((val << 56) & 0xFF00000000000000ULL);
                write_u64(ctx, addr, val);
            }
            break;
            
        // --- Atomic operations (per-thread reservation) ---
        case 20: // lwarx (load word and reserve)
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u32(ctx, addr);
                // Set per-thread reservation
                memory_->set_reservation(ctx.thread_id, addr, 4);
            }
            break;
            
        case 84: // ldarx (load doubleword and reserve)
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                ctx.gpr[d.rd] = read_u64(ctx, addr);
                // Set per-thread reservation
                memory_->set_reservation(ctx.thread_id, addr, 8);
            }
            break;
            
        case 150: // stwcx. (store word conditional)
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                // Check per-thread reservation
                bool success = memory_->check_reservation(ctx.thread_id, addr, 4);
                if (success) {
                    write_u32(ctx, addr, static_cast<u32>(ctx.gpr[d.rs]));
                }
                // Set CR0: [lt, gt, eq, so] = [0, 0, success, xer.so]
                u8 cr0_byte = (success ? 0x2 : 0) | (ctx.xer.so ? 0x1 : 0);
                ctx.cr[0].from_byte(cr0_byte);
                // Clear this thread's reservation (success or failure)
                memory_->clear_reservation(ctx.thread_id);
            }
            break;
            
        case 214: // stdcx. (store doubleword conditional)
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                // Check per-thread reservation
                if (memory_->check_reservation(ctx.thread_id, addr, 8)) {
                    write_u64(ctx, addr, ctx.gpr[d.rs]);
                    ctx.cr[0].eq = true;
                } else {
                    ctx.cr[0].eq = false;
                }
                ctx.cr[0].lt = false;
                ctx.cr[0].gt = false;
                ctx.cr[0].so = ctx.xer.so;
                // Clear this thread's reservation (success or failure)
                memory_->clear_reservation(ctx.thread_id);
            }
            break;
            
        // --- Float indexed load/store ---
        case 535: // lfsx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u32 fval = read_u32(ctx, addr);
                float f = *reinterpret_cast<float*>(&fval);
                ctx.fpr[d.rd] = static_cast<f64>(f);
            }
            break;
            
        case 567: // lfsux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                u32 fval = read_u32(ctx, addr);
                float f = *reinterpret_cast<float*>(&fval);
                ctx.fpr[d.rd] = static_cast<f64>(f);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 599: // lfdx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u64 dval = read_u64(ctx, addr);
                ctx.fpr[d.rd] = *reinterpret_cast<f64*>(&dval);
            }
            break;
            
        case 631: // lfdux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                u64 dval = read_u64(ctx, addr);
                ctx.fpr[d.rd] = *reinterpret_cast<f64*>(&dval);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 663: // stfsx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                float f = static_cast<float>(ctx.fpr[d.rs]);
                u32 fval = *reinterpret_cast<u32*>(&f);
                write_u32(ctx, addr, fval);
            }
            break;
            
        case 695: // stfsux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                float f = static_cast<float>(ctx.fpr[d.rs]);
                u32 fval = *reinterpret_cast<u32*>(&f);
                write_u32(ctx, addr, fval);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 727: // stfdx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u64 dval = *reinterpret_cast<u64*>(&ctx.fpr[d.rs]);
                write_u64(ctx, addr, dval);
            }
            break;
            
        case 759: // stfdux
            {
                GuestAddr addr = ctx.gpr[d.ra] + ctx.gpr[d.rb];
                u64 dval = *reinterpret_cast<u64*>(&ctx.fpr[d.rs]);
                write_u64(ctx, addr, dval);
                ctx.gpr[d.ra] = addr;
            }
            break;
            
        case 983: // stfiwx (store float as integer word)
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u64 dval = *reinterpret_cast<u64*>(&ctx.fpr[d.rs]);
                write_u32(ctx, addr, static_cast<u32>(dval));
            }
            break;
            
        // --- SPR access ---
        case 339: // mfspr
            {
                u32 spr = ((d.raw >> 16) & 0x1F) | ((d.raw >> 6) & 0x3E0);
                switch (spr) {
                    case 1: ctx.gpr[d.rd] = ctx.xer.to_u32(); break;
                    case 8: ctx.gpr[d.rd] = ctx.lr; break;
                    case 9: ctx.gpr[d.rd] = ctx.ctr; break;
                    case 268: // TBL
                    case 284: // TBL (alternate)
                        ctx.gpr[d.rd] = memory_->get_time_base() & 0xFFFFFFFF;
                        break;
                    case 269: // TBU
                    case 285: // TBU (alternate)
                        ctx.gpr[d.rd] = memory_->get_time_base() >> 32;
                        break;
                    case 287: // PVR (processor version)
                        ctx.gpr[d.rd] = 0x710800;  // Xbox 360 Xenon
                        break;
                    default:
                        ctx.gpr[d.rd] = 0;
                        break;
                }
            }
            break;
            
        case 467: // mtspr
            {
                u32 spr = ((d.raw >> 16) & 0x1F) | ((d.raw >> 6) & 0x3E0);
                switch (spr) {
                    case 1: ctx.xer.from_u32(static_cast<u32>(ctx.gpr[d.rs])); break;
                    case 8: ctx.lr = ctx.gpr[d.rs]; break;
                    case 9: ctx.ctr = static_cast<u32>(ctx.gpr[d.rs]); break;  // 32-bit mode
                    default:
                        // Other SPRs are supervisor-only or ignored
                        break;
                }
            }
            break;
            
        case 19: // mfcr
            {
                u32 cr = 0;
                for (int i = 0; i < 8; i++) {
                    cr |= ctx.cr[i].to_byte() << (28 - i * 4);
                }
                ctx.gpr[d.rd] = cr;
            }
            break;
            
        case 144: // mtcrf
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
            
        case 371: // mftb - Move From Time Base
            {
                u32 tbr = ((d.raw >> 16) & 0x1F) | ((d.raw >> 6) & 0x3E0);
                switch (tbr) {
                    case 268: // TBL
                        ctx.gpr[d.rd] = memory_->get_time_base() & 0xFFFFFFFF;
                        break;
                    case 269: // TBU
                        ctx.gpr[d.rd] = memory_->get_time_base() >> 32;
                        break;
                    default:
                        ctx.gpr[d.rd] = 0;
                        break;
                }
            }
            break;
            
        // --- Memory Barrier Operations ---
        case 598: // sync (full barrier) or lwsync (if L=1)
            {
                // Check L bit (bit 9 of instruction) to distinguish sync from lwsync
                // sync L=0: full barrier, sync L=1: lightweight sync (lwsync)
                u32 L = (d.raw >> 21) & 0x1;
                if (L == 0) {
                    // Full sync - all memory operations before sync complete before
                    // any memory operations after sync
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                } else {
                    // lwsync - provides acquire-release semantics
                    // Loads before lwsync complete before loads/stores after
                    // Stores before lwsync complete before stores after
                    std::atomic_thread_fence(std::memory_order_acq_rel);
                }
            }
            break;
            
        case 854: // eieio (Enforce In-Order Execution of I/O)
            // For memory-mapped I/O, ensures all prior stores to MMIO complete
            // before subsequent MMIO accesses
            std::atomic_thread_fence(std::memory_order_release);
            break;
            
        case 86: // dcbf
        case 54: // dcbst
        case 278: // dcbt
        case 246: // dcbtst
        case 470: // dcbi
            // Cache hints - no-op
            break;
            
        case 1014: // dcbz
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                addr &= ~31; // Align to 32 bytes
                memory_->zero_bytes(addr, 32);
            }
            break;
            
        case 982: // icbi
            // Instruction cache invalidate - would invalidate JIT blocks
            break;
            
        // --- String Instructions ---
        case 597: // lswi - Load String Word Immediate
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0);
                u32 nb = d.rb ? d.rb : 32; // 0 means 32 bytes
                u32 r = d.rd;
                u32 i = 0;
                while (nb > 0) {
                    if (i == 0) {
                        ctx.gpr[r] = 0;
                    }
                    u8 byte = read_u8(ctx, addr++);
                    ctx.gpr[r] |= static_cast<u64>(byte) << (56 - i * 8);
                    i++;
                    nb--;
                    if (i == 8) {
                        i = 0;
                        r = (r + 1) % 32;
                    }
                }
            }
            break;
            
        case 533: // lswx - Load String Word Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u32 nb = ctx.xer.byte_count;
                u32 r = d.rd;
                u32 i = 0;
                while (nb > 0) {
                    if (i == 0) {
                        ctx.gpr[r] = 0;
                    }
                    u8 byte = read_u8(ctx, addr++);
                    ctx.gpr[r] |= static_cast<u64>(byte) << (56 - i * 8);
                    i++;
                    nb--;
                    if (i == 8) {
                        i = 0;
                        r = (r + 1) % 32;
                    }
                }
            }
            break;
            
        case 661: // stswi - Store String Word Immediate
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0);
                u32 nb = d.rb ? d.rb : 32; // 0 means 32 bytes
                u32 r = d.rs;
                u32 i = 0;
                while (nb > 0) {
                    u8 byte = (ctx.gpr[r] >> (56 - i * 8)) & 0xFF;
                    write_u8(ctx, addr++, byte);
                    i++;
                    nb--;
                    if (i == 8) {
                        i = 0;
                        r = (r + 1) % 32;
                    }
                }
            }
            break;
            
        case 725: // stswx - Store String Word Indexed
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                u32 nb = ctx.xer.byte_count;
                u32 r = d.rs;
                u32 i = 0;
                while (nb > 0) {
                    u8 byte = (ctx.gpr[r] >> (56 - i * 8)) & 0xFF;
                    write_u8(ctx, addr++, byte);
                    i++;
                    nb--;
                    if (i == 8) {
                        i = 0;
                        r = (r + 1) % 32;
                    }
                }
            }
            break;
            
        // --- Vector load/store (basic) ---
        case 103: // lvx
        case 359: // lvxl
            {
                GuestAddr addr = ((d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb]) & ~15;
                for (int i = 0; i < 16; i++) {
                    ctx.vr[d.rd].u8x16[15 - i] = read_u8(ctx, addr + i);
                }
            }
            break;
            
        case 231: // stvx
        case 487: // stvxl
            {
                GuestAddr addr = ((d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb]) & ~15;
                for (int i = 0; i < 16; i++) {
                    write_u8(ctx, addr + i, ctx.vr[d.rs].u8x16[15 - i]);
                }
            }
            break;
            
        case 7: // lvebx
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                int idx = addr & 15;
                ctx.vr[d.rd].u8x16[15 - idx] = read_u8(ctx, addr);
            }
            break;
            
        case 39: // lvehx
            {
                GuestAddr addr = ((d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb]) & ~1;
                int idx = (addr >> 1) & 7;
                ctx.vr[d.rd].u16x8[7 - idx] = read_u16(ctx, addr);
            }
            break;
            
        case 71: // lvewx
            {
                GuestAddr addr = ((d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb]) & ~3;
                int idx = (addr >> 2) & 3;
                ctx.vr[d.rd].u32x4[3 - idx] = read_u32(ctx, addr);
            }
            break;
            
        case 6: // lvsl
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                int sh = addr & 15;
                for (int i = 0; i < 16; i++) {
                    ctx.vr[d.rd].u8x16[15 - i] = (sh + i) & 0x1F;
                }
            }
            break;
            
        case 38: // lvsr
            {
                GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ctx.gpr[d.rb];
                int sh = addr & 15;
                for (int i = 0; i < 16; i++) {
                    ctx.vr[d.rd].u8x16[15 - i] = (16 - sh + i) & 0x1F;
                }
            }
            break;
            
        default:
            LOGE("Unhandled ext31 opcode: %d at 0x%08llX", d.xo, ctx.pc);
            break;
    }
    
    // Update CR0 if Rc bit set (except for compare/SPR/cache/atomic ops that handle CR themselves)
    // Excluded: CMP=0, CMPL=32, stwcx.=150, stdcx.=214
    if (d.rc && d.xo != 0 && d.xo != 32 && d.xo != 150 && d.xo != 214 && d.xo < 300) {
        update_cr0(ctx, static_cast<s64>(ctx.gpr[d.rd]));
    }
}

//=============================================================================
// 64-bit Load/Store (opcode 58/62)
//=============================================================================

void Interpreter::exec_load_store_ds(ThreadContext& ctx, const DecodedInst& d) {
    // DS-form: 16-bit displacement with low 2 bits as sub-opcode
    s64 ds = d.simm & ~3;
    u32 xo = d.raw & 3;
    GuestAddr addr = (d.ra ? ctx.gpr[d.ra] : 0) + ds;
    
    if (d.opcode == 58) { // Load
        switch (xo) {
            case 0: // ld
                ctx.gpr[d.rd] = read_u64(ctx, addr);
                break;
            case 1: // ldu
                ctx.gpr[d.rd] = read_u64(ctx, addr);
                ctx.gpr[d.ra] = addr;
                break;
            case 2: // lwa
                ctx.gpr[d.rd] = static_cast<s64>(static_cast<s32>(read_u32(ctx, addr)));
                break;
        }
    } else { // Store (opcode 62)
        switch (xo) {
            case 0: // std
                write_u64(ctx, addr, ctx.gpr[d.rs]);
                break;
            case 1: // stdu
                write_u64(ctx, addr, ctx.gpr[d.rs]);
                ctx.gpr[d.ra] = addr;
                break;
        }
    }
}

//=============================================================================
// 64-bit Rotate (opcode 30)
//=============================================================================

void Interpreter::exec_rotate64(ThreadContext& ctx, const DecodedInst& d) {
    u64 rs = ctx.gpr[d.rs];
    u32 xo = (d.raw >> 1) & 0xF;
    u32 sh = ((d.raw >> 11) & 0x1F) | ((d.raw & 2) << 4); // 6-bit shift
    u32 mb = ((d.raw >> 6) & 0x1F) | ((d.raw & 0x20)); // 6-bit mb
    u64 result;
    
    switch (xo) {
        case 0: // rldicl - rotate left, clear left
            {
                u64 rot = rotl64(rs, sh);
                u64 mask = (0xFFFFFFFFFFFFFFFFULL >> mb);
                result = rot & mask;
            }
            break;
            
        case 1: // rldicr - rotate left, clear right
            {
                u32 me = ((d.raw >> 6) & 0x1F) | ((d.raw & 0x20));
                u64 rot = rotl64(rs, sh);
                u64 mask = (0xFFFFFFFFFFFFFFFFULL << (63 - me));
                result = rot & mask;
            }
            break;
            
        case 2: // rldic - rotate left, clear left and right
            {
                u64 rot = rotl64(rs, sh);
                u64 mask = mask64(mb, 63 - sh);
                result = rot & mask;
            }
            break;
            
        case 3: // rldimi - rotate left, insert
            {
                u64 rot = rotl64(rs, sh);
                u64 mask = mask64(mb, 63 - sh);
                result = (rot & mask) | (ctx.gpr[d.ra] & ~mask);
            }
            break;
            
        case 8: // rldcl - rotate left, clear left (variable)
            {
                u32 rb_sh = ctx.gpr[d.rb] & 63;
                u64 rot = rotl64(rs, rb_sh);
                u64 mask = (0xFFFFFFFFFFFFFFFFULL >> mb);
                result = rot & mask;
            }
            break;
            
        case 9: // rldcr - rotate left, clear right (variable)
            {
                u32 me = ((d.raw >> 6) & 0x1F) | ((d.raw & 0x20));
                u32 rb_sh = ctx.gpr[d.rb] & 63;
                u64 rot = rotl64(rs, rb_sh);
                u64 mask = (0xFFFFFFFFFFFFFFFFULL << (63 - me));
                result = rot & mask;
            }
            break;
            
        default:
            LOGE("Unknown rld variant: %d", xo);
            result = rs;
            break;
    }
    
    ctx.gpr[d.ra] = result;
    
    if (d.rc) {
        update_cr0(ctx, static_cast<s64>(result));
    }
}

//=============================================================================
// Complete Float Operations (opcode 59/63)
//=============================================================================

void Interpreter::exec_float_complete(ThreadContext& ctx, const DecodedInst& d) {
    f64 fra = ctx.fpr[d.ra];
    f64 frb = ctx.fpr[d.rb];
    f64 frc = ctx.fpr[(d.raw >> 6) & 0x1F]; // FRC field
    f64 result = 0.0;
    
    // Extended opcode depends on instruction form
    u32 xo_a = (d.raw >> 1) & 0x1F;   // A-form (5 bits)
    u32 xo_x = (d.raw >> 1) & 0x3FF;  // X-form (10 bits)
    
    // Check A-form first (multiply-add family)
    switch (xo_a) {
        case 21: // fadd[s]
            result = fra + frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 20: // fsub[s]
            result = fra - frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 25: // fmul[s]
            result = fra * frc;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 18: // fdiv[s]
            result = fra / frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 29: // fmadd[s]
            result = fra * frc + frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 28: // fmsub[s]
            result = fra * frc - frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 31: // fnmadd[s]
            result = -(fra * frc + frb);
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 30: // fnmsub[s]
            result = -(fra * frc - frb);
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 22: // fsqrt[s]
            result = std::sqrt(frb);
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 24: // fres (reciprocal estimate)
            result = 1.0 / frb;
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 26: // frsqrte (reciprocal sqrt estimate)
            result = 1.0 / std::sqrt(frb);
            ctx.fpr[d.rd] = result;
            goto done;
            
        case 23: // fsel
            result = (fra >= 0.0) ? frc : frb;
            ctx.fpr[d.rd] = result;
            goto done;
    }
    
    // X-form operations
    switch (xo_x) {
        case 0: // fcmpu
            {
                CRField& cr = ctx.cr[d.crfd];
                if (std::isnan(fra) || std::isnan(frb)) {
                    cr.lt = false; cr.gt = false; cr.eq = false; cr.so = true;
                } else if (fra < frb) {
                    cr.lt = true; cr.gt = false; cr.eq = false; cr.so = false;
                } else if (fra > frb) {
                    cr.lt = false; cr.gt = true; cr.eq = false; cr.so = false;
                } else {
                    cr.lt = false; cr.gt = false; cr.eq = true; cr.so = false;
                }
            }
            return;
            
        case 32: // fcmpo
            {
                CRField& cr = ctx.cr[d.crfd];
                if (std::isnan(fra) || std::isnan(frb)) {
                    cr.lt = false; cr.gt = false; cr.eq = false; cr.so = true;
                    // Would set FPSCR[VXSNAN] for signaling NaN
                } else if (fra < frb) {
                    cr.lt = true; cr.gt = false; cr.eq = false; cr.so = false;
                } else if (fra > frb) {
                    cr.lt = false; cr.gt = true; cr.eq = false; cr.so = false;
                } else {
                    cr.lt = false; cr.gt = false; cr.eq = true; cr.so = false;
                }
            }
            return;
            
        case 12: // frsp (round to single)
            result = static_cast<f64>(static_cast<float>(frb));
            break;
            
        case 14: // fctiw (convert to integer word)
            {
                s32 ival;
                if (std::isnan(frb)) {
                    ival = 0x80000000;
                } else if (frb >= 2147483647.0) {
                    ival = 0x7FFFFFFF;
                } else if (frb <= -2147483648.0) {
                    ival = 0x80000000;
                } else {
                    ival = static_cast<s32>(std::trunc(frb));
                }
                u64 bits;
                memcpy(&bits, &ctx.fpr[d.rd], 8);
                bits = (bits & 0xFFFFFFFF00000000ULL) | static_cast<u32>(ival);
                memcpy(&ctx.fpr[d.rd], &bits, 8);
            }
            return;
            
        case 15: // fctiwz (convert to integer word, round toward zero)
            {
                s32 ival;
                if (std::isnan(frb)) {
                    ival = 0x80000000;
                } else if (frb >= 2147483647.0) {
                    ival = 0x7FFFFFFF;
                } else if (frb <= -2147483648.0) {
                    ival = 0x80000000;
                } else {
                    ival = static_cast<s32>(std::trunc(frb));
                }
                u64 bits = static_cast<u32>(ival);
                memcpy(&ctx.fpr[d.rd], &bits, 8);
            }
            return;
            
        case 814: // fctid (convert to integer doubleword)
            {
                s64 ival;
                if (std::isnan(frb)) {
                    ival = 0x8000000000000000LL;
                } else if (frb >= 9223372036854775807.0) {
                    ival = 0x7FFFFFFFFFFFFFFFLL;
                } else if (frb <= -9223372036854775808.0) {
                    ival = 0x8000000000000000LL;
                } else {
                    ival = static_cast<s64>(std::trunc(frb));
                }
                memcpy(&ctx.fpr[d.rd], &ival, 8);
            }
            return;
            
        case 815: // fctidz
            {
                s64 ival;
                if (std::isnan(frb)) {
                    ival = 0x8000000000000000LL;
                } else if (frb >= 9223372036854775807.0) {
                    ival = 0x7FFFFFFFFFFFFFFFLL;
                } else if (frb <= -9223372036854775808.0) {
                    ival = 0x8000000000000000LL;
                } else {
                    ival = static_cast<s64>(std::trunc(frb));
                }
                memcpy(&ctx.fpr[d.rd], &ival, 8);
            }
            return;
            
        case 846: // fcfid (convert from integer doubleword)
            {
                s64 ival;
                memcpy(&ival, &ctx.fpr[d.rb], 8);
                result = static_cast<f64>(ival);
            }
            break;
            
        case 40: // fneg
            result = -frb;
            break;
            
        case 264: // fabs
            result = std::fabs(frb);
            break;
            
        case 136: // fnabs
            result = -std::fabs(frb);
            break;
            
        case 72: // fmr
            result = frb;
            break;
            
        case 64: // mcrfs (move CR from FPSCR)
            ctx.cr[d.crfd].from_byte((ctx.fpscr >> (28 - d.crfs * 4)) & 0xF);
            return;
            
        case 583: // mffs
            {
                u64 fpscr64 = ctx.fpscr;
                memcpy(&ctx.fpr[d.rd], &fpscr64, 8);
            }
            return;
            
        case 711: // mtfsf
            {
                u64 val;
                memcpy(&val, &ctx.fpr[d.rb], 8);
                u32 fm = (d.raw >> 17) & 0xFF;
                for (int i = 0; i < 8; i++) {
                    if (fm & (0x80 >> i)) {
                        u32 mask = 0xF << (28 - i * 4);
                        ctx.fpscr = (ctx.fpscr & ~mask) | (static_cast<u32>(val) & mask);
                    }
                }
            }
            return;
            
        case 70: // mtfsb0
            ctx.fpscr &= ~(1 << (31 - d.crfd));
            return;
            
        case 38: // mtfsb1
            ctx.fpscr |= (1 << (31 - d.crfd));
            return;
            
        default:
            LOGE("Unhandled float xo: %d at 0x%08llX", xo_x, ctx.pc);
            return;
    }
    
done:
    ctx.fpr[d.rd] = result;
    
    if (d.rc) {
        update_cr1(ctx);
    }
}

} // namespace x360mu

