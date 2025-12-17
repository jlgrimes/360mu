/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * ARM64 Code Emitter Implementation
 * 
 * Generates native ARM64 machine code for the JIT compiler
 */

#include "jit.h"
#include <cstring>
#include <stdexcept>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-jit"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGE(...) fprintf(stderr, "[JIT ERROR] " __VA_ARGS__)
#endif

namespace x360mu {

ARM64Emitter::ARM64Emitter(u8* buffer, size_t capacity)
    : buffer_(buffer)
    , current_(buffer)
    , capacity_(capacity)
{
}

void ARM64Emitter::emit32(u32 value) {
    if (current_ + 4 > buffer_ + capacity_) {
        LOGE("JIT code buffer overflow!");
        return;
    }
    *reinterpret_cast<u32*>(current_) = value;
    current_ += 4;
}

//=============================================================================
// Data Processing - Immediate
//=============================================================================

void ARM64Emitter::ADD_imm(int rd, int rn, u32 imm12, bool shift) {
    // ADD Xd, Xn, #imm{, LSL #12}
    u32 sh = shift ? 1 : 0;
    emit32(0x91000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

void ARM64Emitter::ADDS_imm(int rd, int rn, u32 imm12, bool shift) {
    // ADDS Xd, Xn, #imm{, LSL #12}
    u32 sh = shift ? 1 : 0;
    emit32(0xB1000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

void ARM64Emitter::SUB_imm(int rd, int rn, u32 imm12, bool shift) {
    // SUB Xd, Xn, #imm{, LSL #12}
    u32 sh = shift ? 1 : 0;
    emit32(0xD1000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

void ARM64Emitter::SUBS_imm(int rd, int rn, u32 imm12, bool shift) {
    // SUBS Xd, Xn, #imm{, LSL #12}
    u32 sh = shift ? 1 : 0;
    emit32(0xF1000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

void ARM64Emitter::CMP_imm(int rn, u32 imm12) {
    // CMP Xn, #imm = SUBS XZR, Xn, #imm
    SUBS_imm(31, rn, imm12);
}

void ARM64Emitter::CMN_imm(int rn, u32 imm12) {
    // CMN Xn, #imm = ADDS XZR, Xn, #imm
    ADDS_imm(31, rn, imm12);
}

void ARM64Emitter::MOVZ(int rd, u16 imm, int shift) {
    // MOVZ Xd, #imm{, LSL #shift}
    u32 hw = shift / 16;
    emit32(0xD2800000 | (hw << 21) | ((u32)imm << 5) | rd);
}

void ARM64Emitter::MOVK(int rd, u16 imm, int shift) {
    // MOVK Xd, #imm{, LSL #shift}
    u32 hw = shift / 16;
    emit32(0xF2800000 | (hw << 21) | ((u32)imm << 5) | rd);
}

void ARM64Emitter::MOVN(int rd, u16 imm, int shift) {
    // MOVN Xd, #imm{, LSL #shift}
    u32 hw = shift / 16;
    emit32(0x92800000 | (hw << 21) | ((u32)imm << 5) | rd);
}

void ARM64Emitter::MOV_imm(int rd, u64 imm) {
    // Load an arbitrary 64-bit immediate
    // Use optimal instruction sequence based on value
    
    if (imm == 0) {
        // MOV Xd, XZR
        emit32(0xAA1F03E0 | rd);
        return;
    }
    
    // Check if can use MOVZ alone
    bool can_movz = false;
    int movz_shift = 0;
    for (int i = 0; i < 4; i++) {
        u16 chunk = (imm >> (i * 16)) & 0xFFFF;
        u64 test = (u64)chunk << (i * 16);
        if (test == imm) {
            can_movz = true;
            movz_shift = i * 16;
            break;
        }
    }
    
    if (can_movz) {
        MOVZ(rd, imm >> movz_shift, movz_shift);
        return;
    }
    
    // Check if can use MOVN alone (for values close to -1)
    u64 not_imm = ~imm;
    for (int i = 0; i < 4; i++) {
        u16 chunk = (not_imm >> (i * 16)) & 0xFFFF;
        u64 test = (u64)chunk << (i * 16);
        if (test == not_imm) {
            MOVN(rd, chunk, i * 16);
            return;
        }
    }
    
    // Use MOVZ + up to 3 MOVKs
    bool first = true;
    for (int i = 0; i < 4; i++) {
        u16 chunk = (imm >> (i * 16)) & 0xFFFF;
        if (chunk != 0 || (i == 0 && first)) {
            if (first) {
                MOVZ(rd, chunk, i * 16);
                first = false;
            } else {
                MOVK(rd, chunk, i * 16);
            }
        }
    }
}

//=============================================================================
// Data Processing - Register
//=============================================================================

void ARM64Emitter::ADD(int rd, int rn, int rm, int shift, int amount) {
    // ADD Xd, Xn, Xm{, shift #amount}
    emit32(0x8B000000 | (shift << 22) | (rm << 16) | (amount << 10) | (rn << 5) | rd);
}

void ARM64Emitter::ADDS(int rd, int rn, int rm) {
    // ADDS Xd, Xn, Xm
    emit32(0xAB000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::SUB(int rd, int rn, int rm, int shift, int amount) {
    // SUB Xd, Xn, Xm{, shift #amount}
    emit32(0xCB000000 | (shift << 22) | (rm << 16) | (amount << 10) | (rn << 5) | rd);
}

void ARM64Emitter::SUBS(int rd, int rn, int rm) {
    // SUBS Xd, Xn, Xm
    emit32(0xEB000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ADC(int rd, int rn, int rm) {
    // ADC Xd, Xn, Xm
    emit32(0x9A000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ADCS(int rd, int rn, int rm) {
    // ADCS Xd, Xn, Xm
    emit32(0xBA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::SBC(int rd, int rn, int rm) {
    // SBC Xd, Xn, Xm
    emit32(0xDA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::SBCS(int rd, int rn, int rm) {
    // SBCS Xd, Xn, Xm
    emit32(0xFA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::NEG(int rd, int rm) {
    // NEG Xd, Xm = SUB Xd, XZR, Xm
    SUB(rd, 31, rm);
}

void ARM64Emitter::CMP(int rn, int rm) {
    // CMP Xn, Xm = SUBS XZR, Xn, Xm
    SUBS(31, rn, rm);
}

void ARM64Emitter::CMN(int rn, int rm) {
    // CMN Xn, Xm = ADDS XZR, Xn, Xm
    ADDS(31, rn, rm);
}

//=============================================================================
// Logical
//=============================================================================

void ARM64Emitter::AND(int rd, int rn, int rm) {
    emit32(0x8A000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ANDS(int rd, int rn, int rm) {
    emit32(0xEA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ORR(int rd, int rn, int rm) {
    emit32(0xAA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ORN(int rd, int rn, int rm) {
    emit32(0xAA200000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::EOR(int rd, int rn, int rm) {
    emit32(0xCA000000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::EON(int rd, int rn, int rm) {
    emit32(0xCA200000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::BIC(int rd, int rn, int rm) {
    emit32(0x8A200000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::BICS(int rd, int rn, int rm) {
    emit32(0xEA200000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::TST(int rn, int rm) {
    // TST Xn, Xm = ANDS XZR, Xn, Xm
    ANDS(31, rn, rm);
}

// Logical immediate encoding is complex - simplified version
void ARM64Emitter::AND_imm(int rd, int rn, u64 imm) {
    u32 n, immr, imms;
    if (encode_logical_imm_params(imm, true, n, immr, imms)) {
        emit32(0x92000000 | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    } else {
        // Fallback: load immediate to temp register and use reg version
        MOV_imm(arm64::X16, imm);
        AND(rd, rn, arm64::X16);
    }
}

void ARM64Emitter::ORR_imm(int rd, int rn, u64 imm) {
    u32 n, immr, imms;
    if (encode_logical_imm_params(imm, true, n, immr, imms)) {
        emit32(0xB2000000 | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    } else {
        MOV_imm(arm64::X16, imm);
        ORR(rd, rn, arm64::X16);
    }
}

void ARM64Emitter::EOR_imm(int rd, int rn, u64 imm) {
    u32 n, immr, imms;
    if (encode_logical_imm_params(imm, true, n, immr, imms)) {
        emit32(0xD2000000 | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    } else {
        MOV_imm(arm64::X16, imm);
        EOR(rd, rn, arm64::X16);
    }
}

//=============================================================================
// Shifts
//=============================================================================

void ARM64Emitter::LSL(int rd, int rn, int rm) {
    // LSLV Xd, Xn, Xm
    emit32(0x9AC02000 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::LSL_imm(int rd, int rn, int shift) {
    // LSL is alias of UBFM
    int imms = 63 - shift;
    int immr = (64 - shift) & 63;
    emit32(0xD3400000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}

void ARM64Emitter::LSR(int rd, int rn, int rm) {
    // LSRV Xd, Xn, Xm
    emit32(0x9AC02400 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::LSR_imm(int rd, int rn, int shift) {
    // LSR is alias of UBFM with imms=63
    emit32(0xD340FC00 | (shift << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ASR(int rd, int rn, int rm) {
    // ASRV Xd, Xn, Xm
    emit32(0x9AC02800 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ASR_imm(int rd, int rn, int shift) {
    // ASR is alias of SBFM with imms=63
    emit32(0x9340FC00 | (shift << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ROR(int rd, int rn, int rm) {
    // RORV Xd, Xn, Xm
    emit32(0x9AC02C00 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::ROR_imm(int rd, int rn, int shift) {
    // ROR is EXTR Xd, Xn, Xn, #shift
    emit32(0x93C00000 | (rn << 16) | (shift << 10) | (rn << 5) | rd);
}

//=============================================================================
// Multiply
//=============================================================================

void ARM64Emitter::MUL(int rd, int rn, int rm) {
    // MUL Xd, Xn, Xm = MADD Xd, Xn, Xm, XZR
    MADD(rd, rn, rm, 31);
}

void ARM64Emitter::MADD(int rd, int rn, int rm, int ra) {
    emit32(0x9B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

void ARM64Emitter::MSUB(int rd, int rn, int rm, int ra) {
    emit32(0x9B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

void ARM64Emitter::SMULL(int rd, int rn, int rm) {
    // SMULL Xd, Wn, Wm (32-bit signed multiply to 64-bit result)
    emit32(0x9B207C00 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::UMULL(int rd, int rn, int rm) {
    // UMULL Xd, Wn, Wm
    emit32(0x9BA07C00 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::SMULH(int rd, int rn, int rm) {
    // SMULH Xd, Xn, Xm (high 64 bits of 128-bit signed product)
    emit32(0x9B407C00 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::UMULH(int rd, int rn, int rm) {
    // UMULH Xd, Xn, Xm
    emit32(0x9BC07C00 | (rm << 16) | (rn << 5) | rd);
}

//=============================================================================
// Divide
//=============================================================================

void ARM64Emitter::SDIV(int rd, int rn, int rm) {
    emit32(0x9AC00C00 | (rm << 16) | (rn << 5) | rd);
}

void ARM64Emitter::UDIV(int rd, int rn, int rm) {
    emit32(0x9AC00800 | (rm << 16) | (rn << 5) | rd);
}

//=============================================================================
// Bit Manipulation
//=============================================================================

void ARM64Emitter::CLZ(int rd, int rn) {
    emit32(0xDAC01000 | (rn << 5) | rd);
}

void ARM64Emitter::CLS(int rd, int rn) {
    emit32(0xDAC01400 | (rn << 5) | rd);
}

void ARM64Emitter::RBIT(int rd, int rn) {
    emit32(0xDAC00000 | (rn << 5) | rd);
}

void ARM64Emitter::REV(int rd, int rn) {
    // REV Xd, Xn (reverse bytes in 64-bit)
    emit32(0xDAC00C00 | (rn << 5) | rd);
}

void ARM64Emitter::REV16(int rd, int rn) {
    emit32(0xDAC00400 | (rn << 5) | rd);
}

void ARM64Emitter::REV32(int rd, int rn) {
    emit32(0xDAC00800 | (rn << 5) | rd);
}

//=============================================================================
// Extension
//=============================================================================

void ARM64Emitter::SXTB(int rd, int rn) {
    // SBFM Xd, Xn, #0, #7
    emit32(0x93401C00 | (rn << 5) | rd);
}

void ARM64Emitter::SXTH(int rd, int rn) {
    // SBFM Xd, Xn, #0, #15
    emit32(0x93403C00 | (rn << 5) | rd);
}

void ARM64Emitter::SXTW(int rd, int rn) {
    // SBFM Xd, Xn, #0, #31
    emit32(0x93407C00 | (rn << 5) | rd);
}

void ARM64Emitter::UXTB(int rd, int rn) {
    // UBFM Xd, Xn, #0, #7
    emit32(0xD3401C00 | (rn << 5) | rd);
}

void ARM64Emitter::UXTH(int rd, int rn) {
    // UBFM Xd, Xn, #0, #15  
    emit32(0xD3403C00 | (rn << 5) | rd);
}

void ARM64Emitter::UXTW(int rd, int rn) {
    // UBFM Xd, Xn, #0, #31
    emit32(0xD3407C00 | (rn << 5) | rd);
}

//=============================================================================
// Conditional Select
//=============================================================================

void ARM64Emitter::CSEL(int rd, int rn, int rm, int cond) {
    emit32(0x9A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

void ARM64Emitter::CSINC(int rd, int rn, int rm, int cond) {
    emit32(0x9A800400 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

void ARM64Emitter::CSINV(int rd, int rn, int rm, int cond) {
    emit32(0xDA800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

void ARM64Emitter::CSNEG(int rd, int rn, int rm, int cond) {
    emit32(0xDA800400 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

void ARM64Emitter::CSET(int rd, int cond) {
    // CSET Xd, cond = CSINC Xd, XZR, XZR, invert(cond)
    CSINC(rd, 31, 31, cond ^ 1);
}

void ARM64Emitter::CSETM(int rd, int cond) {
    // CSETM Xd, cond = CSINV Xd, XZR, XZR, invert(cond)
    CSINV(rd, 31, 31, cond ^ 1);
}

//=============================================================================
// Load/Store
//=============================================================================

void ARM64Emitter::LDR(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 32760 && (offset & 7) == 0) {
        // Unsigned offset form
        emit32(0xF9400000 | ((offset >> 3) << 10) | (rn << 5) | rt);
    } else if (offset >= -256 && offset <= 255) {
        // Signed offset form (unscaled)
        emit32(0xF8400000 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
    } else {
        // Use temp register for large offset
        MOV_imm(arm64::X16, offset);
        LDR_reg(rt, rn, arm64::X16);
    }
}

void ARM64Emitter::LDRB(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 4096) {
        emit32(0x39400000 | (offset << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x38606800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::LDRH(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 8190 && (offset & 1) == 0) {
        emit32(0x79400000 | ((offset >> 1) << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x78606800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::LDRSB(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 4096) {
        emit32(0x39800000 | (offset << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x38A06800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::LDRSH(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 8190 && (offset & 1) == 0) {
        emit32(0x79800000 | ((offset >> 1) << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x78A06800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::LDRSW(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 16380 && (offset & 3) == 0) {
        emit32(0xB9800000 | ((offset >> 2) << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0xB8A06800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::LDR_reg(int rt, int rn, int rm, int extend, bool shift) {
    u32 s = shift ? 1 : 0;
    u32 option = extend ? extend : 3; // Default: LSL
    emit32(0xF8600800 | (rm << 16) | (option << 13) | (s << 12) | (rn << 5) | rt);
}

void ARM64Emitter::LDP(int rt1, int rt2, int rn, s32 offset) {
    s32 imm7 = offset >> 3;
    emit32(0xA9400000 | ((imm7 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

void ARM64Emitter::STR(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 32760 && (offset & 7) == 0) {
        emit32(0xF9000000 | ((offset >> 3) << 10) | (rn << 5) | rt);
    } else if (offset >= -256 && offset <= 255) {
        emit32(0xF8000000 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        STR_reg(rt, rn, arm64::X16);
    }
}

void ARM64Emitter::STRB(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 4096) {
        emit32(0x39000000 | (offset << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x38206800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::STRH(int rt, int rn, s32 offset) {
    if (offset >= 0 && offset < 8190 && (offset & 1) == 0) {
        emit32(0x79000000 | ((offset >> 1) << 10) | (rn << 5) | rt);
    } else {
        MOV_imm(arm64::X16, offset);
        emit32(0x78206800 | (arm64::X16 << 16) | (rn << 5) | rt);
    }
}

void ARM64Emitter::STR_reg(int rt, int rn, int rm, int extend, bool shift) {
    u32 s = shift ? 1 : 0;
    u32 option = extend ? extend : 3;
    emit32(0xF8200800 | (rm << 16) | (option << 13) | (s << 12) | (rn << 5) | rt);
}

void ARM64Emitter::STP(int rt1, int rt2, int rn, s32 offset) {
    s32 imm7 = offset >> 3;
    emit32(0xA9000000 | ((imm7 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

void ARM64Emitter::LDR_pre(int rt, int rn, s32 offset) {
    emit32(0xF8400C00 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
}

void ARM64Emitter::LDR_post(int rt, int rn, s32 offset) {
    emit32(0xF8400400 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
}

void ARM64Emitter::STR_pre(int rt, int rn, s32 offset) {
    emit32(0xF8000C00 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
}

void ARM64Emitter::STR_post(int rt, int rn, s32 offset) {
    emit32(0xF8000400 | ((offset & 0x1FF) << 12) | (rn << 5) | rt);
}

//=============================================================================
// Branch
//=============================================================================

void ARM64Emitter::B(s32 offset) {
    s32 imm26 = offset >> 2;
    emit32(0x14000000 | (imm26 & 0x03FFFFFF));
}

void ARM64Emitter::B_cond(int cond, s32 offset) {
    s32 imm19 = offset >> 2;
    emit32(0x54000000 | ((imm19 & 0x7FFFF) << 5) | cond);
}

void ARM64Emitter::BL(s32 offset) {
    s32 imm26 = offset >> 2;
    emit32(0x94000000 | (imm26 & 0x03FFFFFF));
}

void ARM64Emitter::BR(int rn) {
    emit32(0xD61F0000 | (rn << 5));
}

void ARM64Emitter::BLR(int rn) {
    emit32(0xD63F0000 | (rn << 5));
}

void ARM64Emitter::RET(int rn) {
    emit32(0xD65F0000 | (rn << 5));
}

void ARM64Emitter::CBZ(int rt, s32 offset) {
    s32 imm19 = offset >> 2;
    emit32(0xB4000000 | ((imm19 & 0x7FFFF) << 5) | rt);
}

void ARM64Emitter::CBNZ(int rt, s32 offset) {
    s32 imm19 = offset >> 2;
    emit32(0xB5000000 | ((imm19 & 0x7FFFF) << 5) | rt);
}

void ARM64Emitter::TBZ(int rt, int bit, s32 offset) {
    s32 imm14 = offset >> 2;
    u32 b40 = bit & 0x1F;
    u32 b5 = (bit >> 5) & 1;
    emit32(0x36000000 | (b5 << 31) | (b40 << 19) | ((imm14 & 0x3FFF) << 5) | rt);
}

void ARM64Emitter::TBNZ(int rt, int bit, s32 offset) {
    s32 imm14 = offset >> 2;
    u32 b40 = bit & 0x1F;
    u32 b5 = (bit >> 5) & 1;
    emit32(0x37000000 | (b5 << 31) | (b40 << 19) | ((imm14 & 0x3FFF) << 5) | rt);
}

//=============================================================================
// System
//=============================================================================

void ARM64Emitter::NOP() {
    emit32(0xD503201F);
}

void ARM64Emitter::BRK(u16 imm) {
    emit32(0xD4200000 | ((u32)imm << 5));
}

void ARM64Emitter::DMB(int option) {
    emit32(0xD50330BF | (option << 8));
}

void ARM64Emitter::DSB(int option) {
    emit32(0xD503309F | (option << 8));
}

void ARM64Emitter::ISB() {
    emit32(0xD5033FDF);
}

void ARM64Emitter::MRS(int rt, u32 sysreg) {
    emit32(0xD5300000 | (sysreg << 5) | rt);
}

void ARM64Emitter::MSR(u32 sysreg, int rt) {
    emit32(0xD5100000 | (sysreg << 5) | rt);
}

//=============================================================================
// NEON
//=============================================================================

void ARM64Emitter::FADD_vec(int vd, int vn, int vm, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x4E20D400 | (sz << 22) | (vm << 16) | (vn << 5) | vd);
}

void ARM64Emitter::FSUB_vec(int vd, int vn, int vm, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x4EA0D400 | (sz << 22) | (vm << 16) | (vn << 5) | vd);
}

void ARM64Emitter::FMUL_vec(int vd, int vn, int vm, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x6E20DC00 | (sz << 22) | (vm << 16) | (vn << 5) | vd);
}

void ARM64Emitter::FDIV_vec(int vd, int vn, int vm, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x6E20FC00 | (sz << 22) | (vm << 16) | (vn << 5) | vd);
}

void ARM64Emitter::FNEG_vec(int vd, int vn, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x6EA0F800 | (sz << 22) | (vn << 5) | vd);
}

void ARM64Emitter::FABS_vec(int vd, int vn, bool is_double) {
    u32 sz = is_double ? 1 : 0;
    emit32(0x4EA0F800 | (sz << 22) | (vn << 5) | vd);
}

void ARM64Emitter::LDR_vec(int vt, int rn, s32 offset) {
    // LDR Qn, [Xn, #offset]
    if (offset >= 0 && offset < 65520 && (offset & 15) == 0) {
        emit32(0x3DC00000 | ((offset >> 4) << 10) | (rn << 5) | vt);
    } else {
        MOV_imm(arm64::X16, offset);
        ADD(arm64::X16, rn, arm64::X16);
        emit32(0x3DC00000 | (arm64::X16 << 5) | vt);
    }
}

void ARM64Emitter::STR_vec(int vt, int rn, s32 offset) {
    // STR Qn, [Xn, #offset]
    if (offset >= 0 && offset < 65520 && (offset & 15) == 0) {
        emit32(0x3D800000 | ((offset >> 4) << 10) | (rn << 5) | vt);
    } else {
        MOV_imm(arm64::X16, offset);
        ADD(arm64::X16, rn, arm64::X16);
        emit32(0x3D800000 | (arm64::X16 << 5) | vt);
    }
}

void ARM64Emitter::DUP_element(int vd, int vn, int index) {
    // DUP Vd.4S, Vn.S[index]
    u32 imm5 = ((index & 3) << 3) | 0x04;
    emit32(0x4E000400 | (imm5 << 16) | (vn << 5) | vd);
}

void ARM64Emitter::DUP_general(int vd, int rn) {
    // DUP Vd.4S, Wn
    emit32(0x4E040C00 | (rn << 5) | vd);
}

void ARM64Emitter::EXT(int vd, int vn, int vm, int index) {
    // EXT Vd.16B, Vn.16B, Vm.16B, #index
    emit32(0x6E000000 | (vm << 16) | ((index & 15) << 11) | (vn << 5) | vd);
}

//=============================================================================
// Address
//=============================================================================

void ARM64Emitter::ADR(int rd, s32 offset) {
    u32 immlo = offset & 3;
    s32 immhi = offset >> 2;
    emit32(0x10000000 | (immlo << 29) | ((immhi & 0x7FFFF) << 5) | rd);
}

void ARM64Emitter::ADRP(int rd, s64 offset) {
    u32 immlo = (offset >> 12) & 3;
    s32 immhi = offset >> 14;
    emit32(0x90000000 | (immlo << 29) | ((immhi & 0x7FFFF) << 5) | rd);
}

//=============================================================================
// Patching
//=============================================================================

void ARM64Emitter::patch_branch(u32* patch_site, void* target) {
    s64 offset = reinterpret_cast<u8*>(target) - reinterpret_cast<u8*>(patch_site);
    s32 imm26 = offset >> 2;
    u32 opcode = *patch_site & 0xFC000000;
    *patch_site = opcode | (imm26 & 0x03FFFFFF);
}

//=============================================================================
// Helpers
//=============================================================================

bool encode_logical_imm_params(u64 imm, bool is_64bit, u32& n, u32& immr, u32& imms) {
    // This is a simplified version - full implementation is complex
    // Returns false if the immediate can't be encoded
    
    if (imm == 0 || imm == ~0ULL) return false;
    
    // Try to find valid encoding
    // For simplicity, we'll return false for complex patterns and use fallback
    
    // Check for simple patterns
    if (is_64bit) {
        // All 1s pattern
        if (imm == 0xFFFFFFFF) {
            n = 0; immr = 0; imms = 31;
            return true;
        }
        // Powers of 2 minus 1
        if ((imm & (imm + 1)) == 0) {
            int bits = __builtin_ctzll(imm + 1);
            n = 1; immr = 0; imms = bits - 1;
            return true;
        }
    }
    
    return false; // Use fallback for other patterns
}

} // namespace x360mu

