/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC instruction decoder
 */

#include "cpu.h"
#include <cstdio>
#include <string>

namespace x360mu {

// Primary opcode table indices
enum PrimaryOpcode {
    OP_TWI = 3,
    OP_MULLI = 7,
    OP_SUBFIC = 8,
    OP_CMPLI = 10,
    OP_CMPI = 11,
    OP_ADDIC = 12,
    OP_ADDIC_RC = 13,
    OP_ADDI = 14,
    OP_ADDIS = 15,
    OP_BC = 16,
    OP_SC = 17,
    OP_B = 18,
    OP_EXT19 = 19,  // CR ops, branches
    OP_RLWIMI = 20,
    OP_RLWINM = 21,
    OP_RLWNM = 23,
    OP_ORI = 24,
    OP_ORIS = 25,
    OP_XORI = 26,
    OP_XORIS = 27,
    OP_ANDI_RC = 28,
    OP_ANDIS_RC = 29,
    OP_EXT30 = 30,  // 64-bit rotate
    OP_EXT31 = 31,  // Integer arithmetic, load/store
    OP_LWZ = 32,
    OP_LWZU = 33,
    OP_LBZ = 34,
    OP_LBZU = 35,
    OP_STW = 36,
    OP_STWU = 37,
    OP_STB = 38,
    OP_STBU = 39,
    OP_LHZ = 40,
    OP_LHZU = 41,
    OP_LHA = 42,
    OP_LHAU = 43,
    OP_STH = 44,
    OP_STHU = 45,
    OP_LMW = 46,
    OP_STMW = 47,
    OP_LFS = 48,
    OP_LFSU = 49,
    OP_LFD = 50,
    OP_LFDU = 51,
    OP_STFS = 52,
    OP_STFSU = 53,
    OP_STFD = 54,
    OP_STFDU = 55,
    OP_LD = 58,     // LD/LDU/LWA (DS-form)
    OP_EXT59 = 59,  // Float single
    OP_STD = 62,    // STD/STDU (DS-form)
    OP_EXT63 = 63,  // Float double
    OP_RLD = 30,    // 64-bit rotate (MD/MDS-form)
    OP_EXT4 = 4,    // VMX128
};

// Extended opcode 31 (common instructions)
enum ExtOp31 {
    XO31_CMP = 0,
    XO31_TW = 4,
    XO31_LVSL = 6,
    XO31_LVEBX = 7,
    XO31_SUBFC = 8,
    XO31_MULHDU = 9,
    XO31_ADDC = 10,
    XO31_MULHWU = 11,
    XO31_MFCR = 19,
    XO31_LWARX = 20,
    XO31_LDX = 21,
    XO31_LWZX = 23,
    XO31_SLW = 24,
    XO31_CNTLZW = 26,
    XO31_SLD = 27,
    XO31_AND = 28,
    XO31_CMPL = 32,
    XO31_LVSR = 38,
    XO31_LVEHX = 39,
    XO31_SUBF = 40,
    XO31_LDUX = 53,
    XO31_DCBST = 54,
    XO31_LWZUX = 55,
    XO31_CNTLZD = 58,
    XO31_ANDC = 60,
    XO31_TD = 68,
    XO31_LVEWX = 71,
    XO31_MULHD = 73,
    XO31_MULHW = 75,
    XO31_MFMSR = 83,
    XO31_LDARX = 84,
    XO31_DCBF = 86,
    XO31_LBZX = 87,
    XO31_LVX = 103,
    XO31_NEG = 104,
    XO31_LBZUX = 119,
    XO31_NOR = 124,
    XO31_SUBFE = 136,
    XO31_ADDE = 138,
    XO31_MTCRF = 144,
    XO31_MTMSR = 146,
    XO31_STDX = 149,
    XO31_STWCX = 150,
    XO31_STWX = 151,
    XO31_STDUX = 181,
    XO31_STWUX = 183,
    XO31_SUBFZE = 200,
    XO31_ADDZE = 202,
    XO31_MTSR = 210,
    XO31_STDCX = 214,
    XO31_STBX = 215,
    XO31_STVX = 231,
    XO31_SUBFME = 232,
    XO31_MULLD = 233,
    XO31_ADDME = 234,
    XO31_MULLW = 235,
    XO31_MTSRIN = 242,
    XO31_DCBTST = 246,
    XO31_STBUX = 247,
    XO31_ADD = 266,
    XO31_DCBT = 278,
    XO31_LHZX = 279,
    XO31_EQV = 284,
    XO31_TLBIE = 306,
    XO31_ECIWX = 310,
    XO31_LHZUX = 311,
    XO31_XOR = 316,
    XO31_MFSPR = 339,
    XO31_LHAX = 343,
    XO31_LVXL = 359,
    XO31_MFTB = 371,
    XO31_LHAUX = 375,
    XO31_STHX = 407,
    XO31_ORC = 412,
    XO31_ECOWX = 438,
    XO31_STHUX = 439,
    XO31_OR = 444,
    XO31_DIVDU = 457,
    XO31_DIVWU = 459,
    XO31_MTSPR = 467,
    XO31_DCBI = 470,
    XO31_NAND = 476,
    XO31_STVXL = 487,
    XO31_DIVD = 489,
    XO31_DIVW = 491,
    XO31_LWBRX = 534,
    XO31_LFSX = 535,
    XO31_SRW = 536,
    XO31_SRD = 539,
    XO31_TLBSYNC = 566,
    XO31_LFSUX = 567,
    XO31_MFSR = 595,
    XO31_LSWI = 597,
    XO31_SYNC = 598,
    XO31_LFDX = 599,
    XO31_LFDUX = 631,
    XO31_MFSRIN = 659,
    XO31_STSWI = 661,
    XO31_STFDX = 727,
    XO31_STFDUX = 759,
    XO31_LHBRX = 790,
    XO31_SRAW = 792,
    XO31_SRAD = 794,
    XO31_SRAWI = 824,
    XO31_SRADI = 826,
    XO31_EIEIO = 854,
    XO31_STHBRX = 918,
    XO31_EXTSH = 922,
    XO31_EXTSB = 954,
    XO31_STFIWX = 983,
    XO31_EXTSW = 986,
    XO31_ICBI = 982,
    XO31_DCBZ = 1014,
};

// Bit extraction helpers
#define BITS(val, start, end) (((val) >> (31 - (end))) & ((1 << ((end) - (start) + 1)) - 1))
#define BIT(val, n) (((val) >> (31 - (n))) & 1)

DecodedInst Decoder::decode(u32 inst) {
    DecodedInst d;
    d.raw = inst;
    d.opcode = BITS(inst, 0, 5);
    d.type = DecodedInst::Type::Unknown;
    
    // Extract common fields
    d.rd = BITS(inst, 6, 10);
    d.rs = d.rd;  // Same position, different meaning
    d.ra = BITS(inst, 11, 15);
    d.rb = BITS(inst, 16, 20);
    d.rc = BIT(inst, 31);
    d.simm = static_cast<s16>(inst & 0xFFFF);
    d.uimm = inst & 0xFFFF;
    
    switch (d.opcode) {
        case OP_ADDI:
        case OP_ADDIS:
            d.type = DecodedInst::Type::Add;
            break;
            
        case OP_SUBFIC:
            d.type = DecodedInst::Type::Sub;
            break;
            
        case OP_ADDIC:
        case OP_ADDIC_RC:
            d.type = DecodedInst::Type::AddCarrying;
            break;
            
        case OP_MULLI:
            d.type = DecodedInst::Type::Mul;
            break;
            
        case OP_CMPI:
            d.type = DecodedInst::Type::CompareLI;
            d.crfd = BITS(inst, 6, 8);
            break;
            
        case OP_CMPLI:
            d.type = DecodedInst::Type::CompareLI;
            d.crfd = BITS(inst, 6, 8);
            break;
            
        case OP_TWI:
            d.type = DecodedInst::Type::TW;
            d.bo = BITS(inst, 6, 10);
            break;
            
        case OP_ORI:
        case OP_ORIS:
            d.type = DecodedInst::Type::Or;
            break;
            
        case OP_XORI:
        case OP_XORIS:
            d.type = DecodedInst::Type::Xor;
            break;
            
        case OP_ANDI_RC:
        case OP_ANDIS_RC:
            d.type = DecodedInst::Type::And;
            break;
            
        case OP_RLWIMI:
        case OP_RLWINM:
        case OP_RLWNM:
            d.type = DecodedInst::Type::Rotate;
            d.sh = BITS(inst, 16, 20);
            d.mb = BITS(inst, 21, 25);
            d.me = BITS(inst, 26, 30);
            break;
            
        case OP_B:
            d.type = DecodedInst::Type::Branch;
            d.li = static_cast<s32>((inst & 0x03FFFFFC) << 6) >> 6;
            break;
            
        case OP_BC:
            d.type = DecodedInst::Type::BranchConditional;
            d.bo = BITS(inst, 6, 10);
            d.bi = BITS(inst, 11, 15);
            d.simm = static_cast<s16>(inst & 0xFFFC);
            break;
            
        case OP_SC:
            d.type = DecodedInst::Type::SC;
            break;
            
        // Load/Store
        case OP_LWZ:
        case OP_LBZ:
        case OP_LHZ:
        case OP_LHA:
        case OP_LFS:
        case OP_LFD:
            d.type = DecodedInst::Type::Load;
            break;
            
        case OP_LWZU:
        case OP_LBZU:
        case OP_LHZU:
        case OP_LHAU:
        case OP_LFSU:
        case OP_LFDU:
            d.type = DecodedInst::Type::LoadUpdate;
            break;
            
        case OP_STW:
        case OP_STB:
        case OP_STH:
        case OP_STFS:
        case OP_STFD:
            d.type = DecodedInst::Type::Store;
            break;
            
        case OP_STWU:
        case OP_STBU:
        case OP_STHU:
        case OP_STFSU:
        case OP_STFDU:
            d.type = DecodedInst::Type::StoreUpdate;
            break;
            
        case OP_LMW:
            d.type = DecodedInst::Type::LoadMultiple;
            break;
            
        case OP_STMW:
            d.type = DecodedInst::Type::StoreMultiple;
            break;
            
        case OP_LD: // 58 - ld/ldu/lwa (DS-form doubleword load)
            d.type = DecodedInst::Type::Load;
            // Low 2 bits determine sub-opcode: 0=ld, 1=ldu, 2=lwa
            break;
            
        case OP_STD: // 62 - std/stdu (DS-form doubleword store)
            d.type = DecodedInst::Type::Store;
            // Low 2 bits determine sub-opcode: 0=std, 1=stdu
            break;
            
        case OP_RLD: // 30 - 64-bit rotate instructions
            d.type = DecodedInst::Type::Rotate;
            // Extract 6-bit shift amount (sh[5] is in bit 1)
            d.sh = BITS(inst, 16, 20) | (BIT(inst, 30) << 5);
            // Extract 6-bit mask begin (mb[5] is in bit 5 of the instruction)
            d.mb = BITS(inst, 21, 25) | (BIT(inst, 26) << 5);
            break;
            
        case OP_EXT19: {
            d.xo = BITS(inst, 21, 30);
            switch (d.xo) {
                case 16: // bclr
                case 528: // bcctr
                    d.type = DecodedInst::Type::BranchConditional;
                    d.bo = BITS(inst, 6, 10);
                    d.bi = BITS(inst, 11, 15);
                    break;
                case 150: // isync
                    d.type = DecodedInst::Type::ISYNC;
                    break;
                default:
                    d.type = DecodedInst::Type::CRLogical;
                    break;
            }
            break;
        }
            
        case OP_EXT31: {
            // X-form instructions use 10-bit XO (bits 21-30)
            // XO-form instructions use 9-bit XO (bits 22-30), but we extract 10 bits
            // since constants like SRD=539, SRAD=794 require 10 bits
            d.xo = BITS(inst, 21, 30);
            
            // Handle common cases
            switch (d.xo) {
                case XO31_ADD:
                case XO31_ADDC:
                case XO31_ADDE:
                case XO31_ADDZE:
                case XO31_ADDME:
                    d.type = DecodedInst::Type::Add;
                    break;
                    
                case XO31_SUBF:
                case XO31_SUBFC:
                case XO31_SUBFE:
                case XO31_SUBFZE:
                case XO31_SUBFME:
                    d.type = DecodedInst::Type::Sub;
                    break;
                    
                case XO31_MULLW:
                case XO31_MULLD:
                case XO31_MULHW:
                case XO31_MULHWU:
                case XO31_MULHD:
                case XO31_MULHDU:
                    d.type = DecodedInst::Type::Mul;
                    break;
                    
                case XO31_DIVW:
                case XO31_DIVWU:
                case XO31_DIVD:
                case XO31_DIVDU:
                    d.type = DecodedInst::Type::Div;
                    break;
                    
                case XO31_AND:
                case XO31_ANDC:
                    d.type = DecodedInst::Type::And;
                    break;
                    
                case XO31_OR:
                case XO31_ORC:
                    d.type = DecodedInst::Type::Or;
                    break;
                    
                case XO31_XOR:
                case XO31_EQV:
                    d.type = DecodedInst::Type::Xor;
                    break;
                    
                case XO31_NOR:
                case XO31_NAND:
                    d.type = DecodedInst::Type::Nand;
                    break;
                    
                case XO31_SLW:
                case XO31_SLD:
                case XO31_SRW:
                case XO31_SRD:
                case XO31_SRAW:
                case XO31_SRAD:
                case XO31_SRAWI:
                case XO31_SRADI:
                    d.type = DecodedInst::Type::Shift;
                    d.sh = BITS(inst, 16, 20);
                    break;
                    
                case XO31_CMP:
                case XO31_CMPL:
                    d.type = DecodedInst::Type::Compare;
                    d.crfd = BITS(inst, 6, 8);
                    break;
                    
                case XO31_LWZX:
                case XO31_LBZX:
                case XO31_LHZX:
                case XO31_LHAX:
                case XO31_LDX:
                case XO31_LFSX:
                case XO31_LFDX:
                    d.type = DecodedInst::Type::Load;
                    break;
                    
                case XO31_LWZUX:
                case XO31_LBZUX:
                case XO31_LHZUX:
                case XO31_LHAUX:
                case XO31_LDUX:
                case XO31_LFSUX:
                case XO31_LFDUX:
                    d.type = DecodedInst::Type::LoadUpdate;
                    break;
                    
                case XO31_STWX:
                case XO31_STBX:
                case XO31_STHX:
                case XO31_STDX:
                case XO31_STFDX:
                    d.type = DecodedInst::Type::Store;
                    break;
                    
                case XO31_STWUX:
                case XO31_STBUX:
                case XO31_STHUX:
                case XO31_STDUX:
                case XO31_STFDUX:
                    d.type = DecodedInst::Type::StoreUpdate;
                    break;
                    
                case XO31_MFSPR:
                    d.type = DecodedInst::Type::MFspr;
                    break;
                    
                case XO31_MTSPR:
                    d.type = DecodedInst::Type::MTspr;
                    break;
                    
                case XO31_MFCR:
                    d.type = DecodedInst::Type::MFcr;
                    break;
                    
                case XO31_MTCRF:
                    d.type = DecodedInst::Type::MTcrf;
                    break;
                    
                case XO31_SYNC:
                    d.type = DecodedInst::Type::SYNC;
                    break;
                    
                case XO31_EIEIO:
                    d.type = DecodedInst::Type::EIEIO;
                    break;
                    
                case XO31_DCBF:
                case XO31_DCBST:
                case XO31_DCBT:
                case XO31_DCBTST:
                case XO31_DCBZ:
                case XO31_DCBI:
                    d.type = DecodedInst::Type::DCBF;
                    break;
                    
                case XO31_ICBI:
                    d.type = DecodedInst::Type::ICBI;
                    break;
                    
                case XO31_CNTLZW:
                case XO31_CNTLZD:
                    d.type = DecodedInst::Type::And; // Uses similar execution path
                    break;
                    
                case XO31_EXTSB:
                case XO31_EXTSH:
                case XO31_EXTSW:
                    d.type = DecodedInst::Type::And;
                    break;
                    
                case XO31_NEG:
                    d.type = DecodedInst::Type::Sub;
                    break;
                    
                case XO31_TW:
                case XO31_TD:
                    d.type = DecodedInst::Type::TW;
                    break;
                    
                // Vector load/store
                case XO31_LVX:
                case XO31_LVXL:
                case XO31_LVEBX:
                case XO31_LVEHX:
                case XO31_LVEWX:
                case XO31_LVSL:
                case XO31_LVSR:
                    d.type = DecodedInst::Type::VLogical; // Vector load
                    break;
                    
                case XO31_STVX:
                case XO31_STVXL:
                    d.type = DecodedInst::Type::VLogical; // Vector store
                    break;
                    
                // Atomic operations
                case XO31_LWARX:
                case XO31_LDARX:
                    d.type = DecodedInst::Type::Load;  // Routed through exec_integer_ext31
                    break;
                    
                case XO31_STWCX:
                case XO31_STDCX:
                    d.type = DecodedInst::Type::Store;  // Routed through exec_integer_ext31
                    break;
            }
            break;
        }
            
        case OP_EXT59: // Float single
        case OP_EXT63: // Float double
            d.xo = BITS(inst, 26, 30);
            // Determine float operation type
            switch (d.xo) {
                case 21: // fadd
                    d.type = DecodedInst::Type::FAdd;
                    break;
                case 20: // fsub
                    d.type = DecodedInst::Type::FSub;
                    break;
                case 25: // fmul
                    d.type = DecodedInst::Type::FMul;
                    break;
                case 18: // fdiv
                    d.type = DecodedInst::Type::FDiv;
                    break;
                case 29: // fmadd
                case 28: // fmsub
                case 31: // fnmadd
                case 30: // fnmsub
                    d.type = DecodedInst::Type::FMadd;
                    break;
                default:
                    if (BITS(inst, 21, 30) == 0) { // fcmp
                        d.type = DecodedInst::Type::FCompare;
                    } else {
                        d.type = DecodedInst::Type::FConvert;
                    }
                    break;
            }
            break;
            
        case OP_EXT4: // VMX128
            d.type = DecodedInst::Type::VLogical;
            // VMX128 instruction decoding is complex, handle in interpreter
            break;
    }
    
    return d;
}

const char* Decoder::get_mnemonic(const DecodedInst& inst) {
    switch (inst.type) {
        case DecodedInst::Type::Add: return "add";
        case DecodedInst::Type::AddCarrying: return "addc";
        case DecodedInst::Type::AddExtended: return "adde";
        case DecodedInst::Type::Sub: return "subf";
        case DecodedInst::Type::SubCarrying: return "subfc";
        case DecodedInst::Type::SubExtended: return "subfe";
        case DecodedInst::Type::Mul: return "mull";
        case DecodedInst::Type::MulHigh: return "mulh";
        case DecodedInst::Type::Div: return "div";
        case DecodedInst::Type::And: return "and";
        case DecodedInst::Type::Or: return "or";
        case DecodedInst::Type::Xor: return "xor";
        case DecodedInst::Type::Nand: return "nand";
        case DecodedInst::Type::Nor: return "nor";
        case DecodedInst::Type::Shift: return "shift";
        case DecodedInst::Type::Rotate: return "rotate";
        case DecodedInst::Type::Compare: return "cmp";
        case DecodedInst::Type::CompareLI: return "cmpi";
        case DecodedInst::Type::Load: return "load";
        case DecodedInst::Type::Store: return "store";
        case DecodedInst::Type::LoadUpdate: return "loadu";
        case DecodedInst::Type::StoreUpdate: return "storeu";
        case DecodedInst::Type::LoadMultiple: return "lmw";
        case DecodedInst::Type::StoreMultiple: return "stmw";
        case DecodedInst::Type::Branch: return "b";
        case DecodedInst::Type::BranchConditional: return "bc";
        case DecodedInst::Type::BranchLink: return "bl";
        case DecodedInst::Type::CRLogical: return "cr";
        case DecodedInst::Type::MTcrf: return "mtcrf";
        case DecodedInst::Type::MFcr: return "mfcr";
        case DecodedInst::Type::MTspr: return "mtspr";
        case DecodedInst::Type::MFspr: return "mfspr";
        case DecodedInst::Type::FAdd: return "fadd";
        case DecodedInst::Type::FSub: return "fsub";
        case DecodedInst::Type::FMul: return "fmul";
        case DecodedInst::Type::FDiv: return "fdiv";
        case DecodedInst::Type::FMadd: return "fmadd";
        case DecodedInst::Type::FNeg: return "fneg";
        case DecodedInst::Type::FAbs: return "fabs";
        case DecodedInst::Type::FCompare: return "fcmp";
        case DecodedInst::Type::FConvert: return "fcvt";
        case DecodedInst::Type::VAdd: return "vadd";
        case DecodedInst::Type::VSub: return "vsub";
        case DecodedInst::Type::VMul: return "vmul";
        case DecodedInst::Type::VDiv: return "vdiv";
        case DecodedInst::Type::VPerm: return "vperm";
        case DecodedInst::Type::VMerge: return "vmerge";
        case DecodedInst::Type::VSplat: return "vsplat";
        case DecodedInst::Type::VCompare: return "vcmp";
        case DecodedInst::Type::VLogical: return "vlogic";
        case DecodedInst::Type::SC: return "sc";
        case DecodedInst::Type::RFI: return "rfi";
        case DecodedInst::Type::ISYNC: return "isync";
        case DecodedInst::Type::TW: return "tw";
        case DecodedInst::Type::TD: return "td";
        case DecodedInst::Type::SYNC: return "sync";
        case DecodedInst::Type::LWSYNC: return "lwsync";
        case DecodedInst::Type::EIEIO: return "eieio";
        case DecodedInst::Type::DCBF: return "dcbf";
        case DecodedInst::Type::DCBST: return "dcbst";
        case DecodedInst::Type::DCBT: return "dcbt";
        case DecodedInst::Type::DCBZ: return "dcbz";
        case DecodedInst::Type::ICBI: return "icbi";
        default: return "unknown";
    }
}

std::string Decoder::disassemble(u32 addr, u32 instruction) {
    char buf[256];
    DecodedInst d = decode(instruction);
    
    snprintf(buf, sizeof(buf), "%08X: %08X  %s",
             addr, instruction, get_mnemonic(d));
    
    return std::string(buf);
}

} // namespace x360mu

