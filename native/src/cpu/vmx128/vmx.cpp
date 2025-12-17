/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * VMX128 instruction implementation
 */

#include "vmx.h"
#include "memory/memory.h"
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-vmx"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...)
#define LOGE(...) fprintf(stderr, "[VMX ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

Vmx128Unit::Vmx128Unit() = default;

// Bit extraction
#define BITS(val, start, end) (((val) >> (31 - (end))) & ((1 << ((end) - (start) + 1)) - 1))

Vmx128Inst Vmx128Unit::decode(u32 inst) {
    Vmx128Inst d;
    d.raw = inst;
    d.opcode = BITS(inst, 0, 5);
    d.vd = BITS(inst, 6, 10);
    d.va = BITS(inst, 11, 15);
    d.vb = BITS(inst, 16, 20);
    d.vc = BITS(inst, 21, 25);
    d.xo = BITS(inst, 21, 31);
    d.rc = inst & 1;
    
    // Xbox 360 extended register encoding (7 bits for 128 registers)
    // This is a simplified version - actual encoding varies by instruction
    d.vd128 = d.vd | ((inst >> 21) & 0x60);
    d.va128 = d.va;
    d.vb128 = d.vb;
    
    // Decode instruction type based on extended opcode
    if (d.opcode == 4) {
        u32 xo_11 = BITS(inst, 21, 31);
        u32 xo_6 = BITS(inst, 26, 31);
        
        switch (xo_6) {
            case 46: d.type = Vmx128Inst::Type::VMaddfp; break;
            case 47: d.type = Vmx128Inst::Type::VNmsubfp; break;
            case 43: d.type = Vmx128Inst::Type::VPerm; break;
        }
        
        switch (xo_11) {
            // Integer add/sub
            case 0: d.type = Vmx128Inst::Type::VAddUbm; break;
            case 64: d.type = Vmx128Inst::Type::VAddUhm; break;
            case 128: d.type = Vmx128Inst::Type::VAddUwm; break;
            case 768: d.type = Vmx128Inst::Type::VAddSbs; break;
            case 832: d.type = Vmx128Inst::Type::VAddShs; break;
            case 896: d.type = Vmx128Inst::Type::VAddSws; break;
            case 1024: d.type = Vmx128Inst::Type::VSubUbm; break;
            case 1088: d.type = Vmx128Inst::Type::VSubUhm; break;
            case 1152: d.type = Vmx128Inst::Type::VSubUwm; break;
            
            // Float
            case 10: d.type = Vmx128Inst::Type::VAddFp; break;
            case 74: d.type = Vmx128Inst::Type::VSubFp; break;
            case 1034: d.type = Vmx128Inst::Type::VMaxfp; break;
            case 1098: d.type = Vmx128Inst::Type::VMinfp; break;
            case 266: d.type = Vmx128Inst::Type::VReciprocalFp; break;
            case 330: d.type = Vmx128Inst::Type::VRsqrteFp; break;
            
            // Compare
            case 198: d.type = Vmx128Inst::Type::VCmpEqFp; break;
            case 454: d.type = Vmx128Inst::Type::VCmpGeFp; break;
            case 710: d.type = Vmx128Inst::Type::VCmpGtFp; break;
            case 134: d.type = Vmx128Inst::Type::VCmpEquw; break;
            case 646: d.type = Vmx128Inst::Type::VCmpGtuw; break;
            case 902: d.type = Vmx128Inst::Type::VCmpGtsw; break;
            
            // Logical
            case 1028: d.type = Vmx128Inst::Type::VAnd; break;
            case 1092: d.type = Vmx128Inst::Type::VAndc; break;
            case 1156: d.type = Vmx128Inst::Type::VOr; break;
            case 1284: d.type = Vmx128Inst::Type::VXor; break;
            case 1220: d.type = Vmx128Inst::Type::VNor; break;
            
            // Merge
            case 12: d.type = Vmx128Inst::Type::VMrghb; break;
            case 76: d.type = Vmx128Inst::Type::VMrghh; break;
            case 140: d.type = Vmx128Inst::Type::VMrghw; break;
            case 268: d.type = Vmx128Inst::Type::VMrglb; break;
            case 332: d.type = Vmx128Inst::Type::VMrglh; break;
            case 396: d.type = Vmx128Inst::Type::VMrglw; break;
            
            // Splat
            case 524: d.type = Vmx128Inst::Type::VSpltb; break;
            case 588: d.type = Vmx128Inst::Type::VSplth; break;
            case 652: d.type = Vmx128Inst::Type::VSpltw; break;
            case 780: d.type = Vmx128Inst::Type::VSpltIsb; break;
            case 844: d.type = Vmx128Inst::Type::VSpltIsh; break;
            case 908: d.type = Vmx128Inst::Type::VSpltIsw; break;
            
            // Shift
            case 260: d.type = Vmx128Inst::Type::VSlb; break;
            case 324: d.type = Vmx128Inst::Type::VSlh; break;
            case 388: d.type = Vmx128Inst::Type::VSlw; break;
            case 516: d.type = Vmx128Inst::Type::VSrb; break;
            case 580: d.type = Vmx128Inst::Type::VSrh; break;
            case 644: d.type = Vmx128Inst::Type::VSrw; break;
            case 772: d.type = Vmx128Inst::Type::VSrab; break;
            case 836: d.type = Vmx128Inst::Type::VSrah; break;
            case 900: d.type = Vmx128Inst::Type::VSraw; break;
            
            // Rotate
            case 4: d.type = Vmx128Inst::Type::VRlb; break;
            case 68: d.type = Vmx128Inst::Type::VRlh; break;
            case 132: d.type = Vmx128Inst::Type::VRlw; break;
            
            // Convert
            case 778: d.type = Vmx128Inst::Type::VCfux; break;
            case 842: d.type = Vmx128Inst::Type::VCfsx; break;
            case 906: d.type = Vmx128Inst::Type::VCtuxs; break;
            case 970: d.type = Vmx128Inst::Type::VCtsxs; break;
            case 522: d.type = Vmx128Inst::Type::VRfin; break;
            case 586: d.type = Vmx128Inst::Type::VRfiz; break;
            case 650: d.type = Vmx128Inst::Type::VRfip; break;
            case 714: d.type = Vmx128Inst::Type::VRfim; break;
            
            // Pack/Unpack
            case 14: d.type = Vmx128Inst::Type::VPkuhum; break;
            case 78: d.type = Vmx128Inst::Type::VPkuwum; break;
            case 526: d.type = Vmx128Inst::Type::VUpkhsb; break;
            case 590: d.type = Vmx128Inst::Type::VUpkhsh; break;
            case 654: d.type = Vmx128Inst::Type::VUpklsb; break;
            case 718: d.type = Vmx128Inst::Type::VUpklsh; break;
            
            // Xbox 360 specific (dot products) - opcodes may vary
            case 112: d.type = Vmx128Inst::Type::VDot3fp; break;
            case 113: d.type = Vmx128Inst::Type::VDot4fp; break;
        }
    }
    
    return d;
}

void Vmx128Unit::execute(ThreadContext& ctx, const Vmx128Inst& inst) {
    VectorReg& vd = ctx.vr[inst.vd128 % cpu::NUM_VMX_REGS];
    const VectorReg& va = ctx.vr[inst.va128 % cpu::NUM_VMX_REGS];
    const VectorReg& vb = ctx.vr[inst.vb128 % cpu::NUM_VMX_REGS];
    const VectorReg& vc = ctx.vr[inst.vc % cpu::NUM_VMX_REGS];
    
    switch (inst.type) {
        // Integer arithmetic
        case Vmx128Inst::Type::VAddUbm: vadd_ubm(vd, va, vb); break;
        case Vmx128Inst::Type::VAddUhm: vadd_uhm(vd, va, vb); break;
        case Vmx128Inst::Type::VAddUwm: vadd_uwm(vd, va, vb); break;
        case Vmx128Inst::Type::VSubUbm: vsub_ubm(vd, va, vb); break;
        case Vmx128Inst::Type::VSubUhm: vsub_uhm(vd, va, vb); break;
        case Vmx128Inst::Type::VSubUwm: vsub_uwm(vd, va, vb); break;
        case Vmx128Inst::Type::VAddSbs: vaddsbs(vd, va, vb); break;
        case Vmx128Inst::Type::VAddShs: vaddshs(vd, va, vb); break;
        case Vmx128Inst::Type::VAddSws: vaddsws(vd, va, vb); break;
        
        // Float arithmetic
        case Vmx128Inst::Type::VAddFp: vaddfp(vd, va, vb); break;
        case Vmx128Inst::Type::VSubFp: vsubfp(vd, va, vb); break;
        case Vmx128Inst::Type::VMulFp: vmulfp(vd, va, vc); break;
        case Vmx128Inst::Type::VMaddfp: vmaddfp(vd, va, vb, vc); break;
        case Vmx128Inst::Type::VNmsubfp: vnmsubfp(vd, va, vb, vc); break;
        case Vmx128Inst::Type::VMaxfp: vmaxfp(vd, va, vb); break;
        case Vmx128Inst::Type::VMinfp: vminfp(vd, va, vb); break;
        case Vmx128Inst::Type::VReciprocalFp: vrefp(vd, vb); break;
        case Vmx128Inst::Type::VRsqrteFp: vrsqrtefp(vd, vb); break;
        
        // Dot products (Xbox 360 extension)
        case Vmx128Inst::Type::VDot3fp: vdot3fp(vd, va, vb); break;
        case Vmx128Inst::Type::VDot4fp: vdot4fp(vd, va, vb); break;
        case Vmx128Inst::Type::VCross3fp: vcross3fp(vd, va, vb); break;
        case Vmx128Inst::Type::VShufD: vshufd(vd, vb, inst.va); break;
        
        // Compare
        case Vmx128Inst::Type::VCmpEqFp: vcmpeqfp(vd, va, vb, inst.rc, ctx); break;
        case Vmx128Inst::Type::VCmpGeFp: vcmpgefp(vd, va, vb, inst.rc, ctx); break;
        case Vmx128Inst::Type::VCmpGtFp: vcmpgtfp(vd, va, vb, inst.rc, ctx); break;
        case Vmx128Inst::Type::VCmpEquw: vcmpequw(vd, va, vb, inst.rc, ctx); break;
        case Vmx128Inst::Type::VCmpGtuw: vcmpgtuw(vd, va, vb, inst.rc, ctx); break;
        case Vmx128Inst::Type::VCmpGtsw: vcmpgtsw(vd, va, vb, inst.rc, ctx); break;
        
        // Logical
        case Vmx128Inst::Type::VAnd: vand(vd, va, vb); break;
        case Vmx128Inst::Type::VAndc: vandc(vd, va, vb); break;
        case Vmx128Inst::Type::VOr: vor(vd, va, vb); break;
        case Vmx128Inst::Type::VOrc: vorc(vd, va, vb); break;
        case Vmx128Inst::Type::VXor: vxor(vd, va, vb); break;
        case Vmx128Inst::Type::VNor: vnor(vd, va, vb); break;
        
        // Permute
        case Vmx128Inst::Type::VPerm: vperm(vd, va, vb, vc); break;
        
        // Merge
        case Vmx128Inst::Type::VMrghb: vmrghb(vd, va, vb); break;
        case Vmx128Inst::Type::VMrghh: vmrghh(vd, va, vb); break;
        case Vmx128Inst::Type::VMrghw: vmrghw(vd, va, vb); break;
        case Vmx128Inst::Type::VMrglb: vmrglb(vd, va, vb); break;
        case Vmx128Inst::Type::VMrglh: vmrglh(vd, va, vb); break;
        case Vmx128Inst::Type::VMrglw: vmrglw(vd, va, vb); break;
        
        // Splat
        case Vmx128Inst::Type::VSpltb: vspltb(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VSplth: vsplth(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VSpltw: vspltw(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VSpltIsb: vspltisb(vd, static_cast<s8>(inst.va)); break;
        case Vmx128Inst::Type::VSpltIsh: vspltish(vd, static_cast<s8>(inst.va)); break;
        case Vmx128Inst::Type::VSpltIsw: vspltisw(vd, static_cast<s8>(inst.va)); break;
        
        // Shift
        case Vmx128Inst::Type::VSlb: vslb(vd, va, vb); break;
        case Vmx128Inst::Type::VSlh: vslh(vd, va, vb); break;
        case Vmx128Inst::Type::VSlw: vslw(vd, va, vb); break;
        case Vmx128Inst::Type::VSrb: vsrb(vd, va, vb); break;
        case Vmx128Inst::Type::VSrh: vsrh(vd, va, vb); break;
        case Vmx128Inst::Type::VSrw: vsrw(vd, va, vb); break;
        case Vmx128Inst::Type::VSrab: vsrab(vd, va, vb); break;
        case Vmx128Inst::Type::VSrah: vsrah(vd, va, vb); break;
        case Vmx128Inst::Type::VSraw: vsraw(vd, va, vb); break;
        
        // Rotate
        case Vmx128Inst::Type::VRlb: vrlb(vd, va, vb); break;
        case Vmx128Inst::Type::VRlh: vrlh(vd, va, vb); break;
        case Vmx128Inst::Type::VRlw: vrlw(vd, va, vb); break;
        
        // Convert
        case Vmx128Inst::Type::VCfux: vcfux(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VCfsx: vcfsx(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VCtuxs: vctuxs(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VCtsxs: vctsxs(vd, vb, inst.va); break;
        case Vmx128Inst::Type::VRfin: vrfin(vd, vb); break;
        case Vmx128Inst::Type::VRfiz: vrfiz(vd, vb); break;
        case Vmx128Inst::Type::VRfip: vrfip(vd, vb); break;
        case Vmx128Inst::Type::VRfim: vrfim(vd, vb); break;
        
        // Pack/Unpack
        case Vmx128Inst::Type::VPkuhum: vpkuhum(vd, va, vb); break;
        case Vmx128Inst::Type::VPkuwum: vpkuwum(vd, va, vb); break;
        case Vmx128Inst::Type::VUpkhsb: vupkhsb(vd, vb); break;
        case Vmx128Inst::Type::VUpkhsh: vupkhsh(vd, vb); break;
        case Vmx128Inst::Type::VUpklsb: vupklsb(vd, vb); break;
        case Vmx128Inst::Type::VUpklsh: vupklsh(vd, vb); break;
        
        default:
            LOGE("Unimplemented VMX instruction type: %d", static_cast<int>(inst.type));
            break;
    }
}

// Remaining non-inline implementations

void Vmx128Unit::vadd_ubm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint8x16_t a = vld1q_u8(va.u8x16);
    uint8x16_t b = vld1q_u8(vb.u8x16);
    uint8x16_t result = vaddq_u8(a, b);
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vadd_uhm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint16x8_t a = vld1q_u16(va.u16x8);
    uint16x8_t b = vld1q_u16(vb.u16x8);
    uint16x8_t result = vaddq_u16(a, b);
    vst1q_u16(vd.u16x8, result);
}

void Vmx128Unit::vsub_ubm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint8x16_t a = vld1q_u8(va.u8x16);
    uint8x16_t b = vld1q_u8(vb.u8x16);
    uint8x16_t result = vsubq_u8(a, b);
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vsub_uhm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint16x8_t a = vld1q_u16(va.u16x8);
    uint16x8_t b = vld1q_u16(vb.u16x8);
    uint16x8_t result = vsubq_u16(a, b);
    vst1q_u16(vd.u16x8, result);
}

void Vmx128Unit::vaddsbs(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    int8x16_t a = vld1q_s8(reinterpret_cast<const s8*>(va.u8x16));
    int8x16_t b = vld1q_s8(reinterpret_cast<const s8*>(vb.u8x16));
    int8x16_t result = vqaddq_s8(a, b);
    vst1q_s8(reinterpret_cast<s8*>(vd.u8x16), result);
}

void Vmx128Unit::vaddshs(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    int16x8_t a = vld1q_s16(reinterpret_cast<const s16*>(va.u16x8));
    int16x8_t b = vld1q_s16(reinterpret_cast<const s16*>(vb.u16x8));
    int16x8_t result = vqaddq_s16(a, b);
    vst1q_s16(reinterpret_cast<s16*>(vd.u16x8), result);
}

void Vmx128Unit::vaddsws(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    int32x4_t a = vld1q_s32(reinterpret_cast<const s32*>(va.u32x4));
    int32x4_t b = vld1q_s32(reinterpret_cast<const s32*>(vb.u32x4));
    int32x4_t result = vqaddq_s32(a, b);
    vst1q_s32(reinterpret_cast<s32*>(vd.u32x4), result);
}

void Vmx128Unit::vnmsubfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t c = vld1q_f32(vc.f32x4);
    // vd = -(a * c - b) = b - a * c
    float32x4_t result = vfmsq_f32(b, a, c);
    vst1q_f32(vd.f32x4, result);
}

void Vmx128Unit::vrefp(VectorReg& vd, const VectorReg& vb) {
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vrecpeq_f32(b);
    // Newton-Raphson refinement for better accuracy
    result = vmulq_f32(vrecpsq_f32(b, result), result);
    vst1q_f32(vd.f32x4, result);
}

void Vmx128Unit::vrsqrtefp(VectorReg& vd, const VectorReg& vb) {
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vrsqrteq_f32(b);
    // Newton-Raphson refinement
    result = vmulq_f32(vrsqrtsq_f32(vmulq_f32(b, result), result), result);
    vst1q_f32(vd.f32x4, result);
}

void Vmx128Unit::vcmpeqfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    uint32x4_t result = vceqq_f32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vcmpgefp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    uint32x4_t result = vcgeq_f32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vcmpgtfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    uint32x4_t result = vcgtq_f32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vcmpequw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vceqq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vcmpgtuw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vcgtq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vcmpgtsw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx) {
    int32x4_t a = vld1q_s32(reinterpret_cast<const s32*>(va.u32x4));
    int32x4_t b = vld1q_s32(reinterpret_cast<const s32*>(vb.u32x4));
    uint32x4_t result = vcgtq_s32(a, b);
    vst1q_u32(vd.u32x4, result);
    
    if (rc) {
        bool all_true = vminvq_u32(result) == 0xFFFFFFFF;
        bool all_false = vmaxvq_u32(result) == 0;
        update_cr6(ctx, all_true, all_false);
    }
}

void Vmx128Unit::vandc(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vbicq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vorc(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vornq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vnor(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vmvnq_u32(vorrq_u32(a, b));
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vperm(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc) {
    // vperm selects bytes from va:vb based on low 5 bits of each byte in vc
    uint8x16_t a = vld1q_u8(va.u8x16);
    uint8x16_t b = vld1q_u8(vb.u8x16);
    uint8x16_t c = vld1q_u8(vc.u8x16);
    
    // Combine va and vb
    // Use low 5 bits as indices (0-31)
    uint8x16_t masked_c = vandq_u8(c, vdupq_n_u8(0x1F));
    
    // Use table lookup - combine a and b into a table
    uint8x16x2_t table = {a, b};
    uint8x16_t result = vqtbl2q_u8(table, masked_c);
    
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vmrghb(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint8x16_t a = vld1q_u8(va.u8x16);
    uint8x16_t b = vld1q_u8(vb.u8x16);
    uint8x8_t a_hi = vget_high_u8(a);
    uint8x8_t b_hi = vget_high_u8(b);
    uint8x16_t result = vcombine_u8(vzip1_u8(a_hi, b_hi), vzip2_u8(a_hi, b_hi));
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vmrghh(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint16x8_t a = vld1q_u16(va.u16x8);
    uint16x8_t b = vld1q_u16(vb.u16x8);
    uint16x4_t a_hi = vget_high_u16(a);
    uint16x4_t b_hi = vget_high_u16(b);
    uint16x8_t result = vcombine_u16(vzip1_u16(a_hi, b_hi), vzip2_u16(a_hi, b_hi));
    vst1q_u16(vd.u16x8, result);
}

void Vmx128Unit::vmrghw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x2_t a_hi = vget_high_u32(a);
    uint32x2_t b_hi = vget_high_u32(b);
    uint32x4_t result = vcombine_u32(vzip1_u32(a_hi, b_hi), vzip2_u32(a_hi, b_hi));
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vmrglb(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint8x16_t a = vld1q_u8(va.u8x16);
    uint8x16_t b = vld1q_u8(vb.u8x16);
    uint8x8_t a_lo = vget_low_u8(a);
    uint8x8_t b_lo = vget_low_u8(b);
    uint8x16_t result = vcombine_u8(vzip1_u8(a_lo, b_lo), vzip2_u8(a_lo, b_lo));
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vmrglh(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint16x8_t a = vld1q_u16(va.u16x8);
    uint16x8_t b = vld1q_u16(vb.u16x8);
    uint16x4_t a_lo = vget_low_u16(a);
    uint16x4_t b_lo = vget_low_u16(b);
    uint16x8_t result = vcombine_u16(vzip1_u16(a_lo, b_lo), vzip2_u16(a_lo, b_lo));
    vst1q_u16(vd.u16x8, result);
}

void Vmx128Unit::vmrglw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x2_t a_lo = vget_low_u32(a);
    uint32x2_t b_lo = vget_low_u32(b);
    uint32x4_t result = vcombine_u32(vzip1_u32(a_lo, b_lo), vzip2_u32(a_lo, b_lo));
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vspltb(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    u8 val = vb.u8x16[uimm & 15];
    uint8x16_t result = vdupq_n_u8(val);
    vst1q_u8(vd.u8x16, result);
}

void Vmx128Unit::vsplth(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    u16 val = vb.u16x8[uimm & 7];
    uint16x8_t result = vdupq_n_u16(val);
    vst1q_u16(vd.u16x8, result);
}

void Vmx128Unit::vspltw(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    u32 val = vb.u32x4[uimm & 3];
    uint32x4_t result = vdupq_n_u32(val);
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vspltisb(VectorReg& vd, s8 simm) {
    int8x16_t result = vdupq_n_s8(simm);
    vst1q_s8(reinterpret_cast<s8*>(vd.u8x16), result);
}

void Vmx128Unit::vspltish(VectorReg& vd, s8 simm) {
    int16x8_t result = vdupq_n_s16(static_cast<s16>(simm));
    vst1q_s16(reinterpret_cast<s16*>(vd.u16x8), result);
}

void Vmx128Unit::vspltisw(VectorReg& vd, s8 simm) {
    int32x4_t result = vdupq_n_s32(static_cast<s32>(simm));
    vst1q_s32(reinterpret_cast<s32*>(vd.u32x4), result);
}

void Vmx128Unit::vslb(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 16; i++) {
        vd.u8x16[i] = va.u8x16[i] << (vb.u8x16[i] & 7);
    }
}

void Vmx128Unit::vslh(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u16x8[i] = va.u16x8[i] << (vb.u16x8[i] & 15);
    }
}

void Vmx128Unit::vslw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    int32x4_t shift = vld1q_s32(reinterpret_cast<const s32*>(vb.u32x4));
    shift = vandq_s32(shift, vdupq_n_s32(31));
    uint32x4_t result = vshlq_u32(a, shift);
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vsrb(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 16; i++) {
        vd.u8x16[i] = va.u8x16[i] >> (vb.u8x16[i] & 7);
    }
}

void Vmx128Unit::vsrh(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u16x8[i] = va.u16x8[i] >> (vb.u16x8[i] & 15);
    }
}

void Vmx128Unit::vsrw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    int32x4_t shift = vld1q_s32(reinterpret_cast<const s32*>(vb.u32x4));
    shift = vnegq_s32(vandq_s32(shift, vdupq_n_s32(31)));
    uint32x4_t result = vshlq_u32(a, shift);
    vst1q_u32(vd.u32x4, result);
}

void Vmx128Unit::vsrab(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 16; i++) {
        vd.u8x16[i] = static_cast<u8>(static_cast<s8>(va.u8x16[i]) >> (vb.u8x16[i] & 7));
    }
}

void Vmx128Unit::vsrah(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u16x8[i] = static_cast<u16>(static_cast<s16>(va.u16x8[i]) >> (vb.u16x8[i] & 15));
    }
}

void Vmx128Unit::vsraw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    int32x4_t a = vld1q_s32(reinterpret_cast<const s32*>(va.u32x4));
    int32x4_t shift = vld1q_s32(reinterpret_cast<const s32*>(vb.u32x4));
    shift = vnegq_s32(vandq_s32(shift, vdupq_n_s32(31)));
    int32x4_t result = vshlq_s32(a, shift);
    vst1q_s32(reinterpret_cast<s32*>(vd.u32x4), result);
}

void Vmx128Unit::vrlb(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 16; i++) {
        u8 shift = vb.u8x16[i] & 7;
        vd.u8x16[i] = (va.u8x16[i] << shift) | (va.u8x16[i] >> (8 - shift));
    }
}

void Vmx128Unit::vrlh(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        u16 shift = vb.u16x8[i] & 15;
        vd.u16x8[i] = (va.u16x8[i] << shift) | (va.u16x8[i] >> (16 - shift));
    }
}

void Vmx128Unit::vrlw(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        u32 shift = vb.u32x4[i] & 31;
        vd.u32x4[i] = (va.u32x4[i] << shift) | (va.u32x4[i] >> (32 - shift));
    }
}

void Vmx128Unit::vcfux(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    float scale = 1.0f / (1 << uimm);
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = static_cast<f32>(vb.u32x4[i]) * scale;
    }
}

void Vmx128Unit::vcfsx(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    float scale = 1.0f / (1 << uimm);
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = static_cast<f32>(static_cast<s32>(vb.u32x4[i])) * scale;
    }
}

void Vmx128Unit::vctuxs(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    float scale = static_cast<float>(1 << uimm);
    for (int i = 0; i < 4; i++) {
        f32 val = vb.f32x4[i] * scale;
        if (val < 0) vd.u32x4[i] = 0;
        else if (val > 0xFFFFFFFF) vd.u32x4[i] = 0xFFFFFFFF;
        else vd.u32x4[i] = static_cast<u32>(val);
    }
}

void Vmx128Unit::vctsxs(VectorReg& vd, const VectorReg& vb, u8 uimm) {
    float scale = static_cast<float>(1 << uimm);
    for (int i = 0; i < 4; i++) {
        f32 val = vb.f32x4[i] * scale;
        if (val < -2147483648.0f) vd.u32x4[i] = 0x80000000;
        else if (val > 2147483647.0f) vd.u32x4[i] = 0x7FFFFFFF;
        else vd.u32x4[i] = static_cast<u32>(static_cast<s32>(val));
    }
}

void Vmx128Unit::vrfin(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = std::nearbyint(vb.f32x4[i]);
    }
}

void Vmx128Unit::vrfiz(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = std::trunc(vb.f32x4[i]);
    }
}

void Vmx128Unit::vrfip(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = std::ceil(vb.f32x4[i]);
    }
}

void Vmx128Unit::vrfim(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.f32x4[i] = std::floor(vb.f32x4[i]);
    }
}

void Vmx128Unit::vpkuhum(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u8x16[i] = static_cast<u8>(va.u16x8[i]);
        vd.u8x16[i + 8] = static_cast<u8>(vb.u16x8[i]);
    }
}

void Vmx128Unit::vpkuwum(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.u16x8[i] = static_cast<u16>(va.u32x4[i]);
        vd.u16x8[i + 4] = static_cast<u16>(vb.u32x4[i]);
    }
}

void Vmx128Unit::vupkhsb(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u16x8[i] = static_cast<u16>(static_cast<s16>(static_cast<s8>(vb.u8x16[i])));
    }
}

void Vmx128Unit::vupkhsh(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.u32x4[i] = static_cast<u32>(static_cast<s32>(static_cast<s16>(vb.u16x8[i])));
    }
}

void Vmx128Unit::vupklsb(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 8; i++) {
        vd.u16x8[i] = static_cast<u16>(static_cast<s16>(static_cast<s8>(vb.u8x16[i + 8])));
    }
}

void Vmx128Unit::vupklsh(VectorReg& vd, const VectorReg& vb) {
    for (int i = 0; i < 4; i++) {
        vd.u32x4[i] = static_cast<u32>(static_cast<s32>(static_cast<s16>(vb.u16x8[i + 4])));
    }
}

void Vmx128Unit::update_cr6(ThreadContext& ctx, bool all_true, bool all_false) {
    ctx.cr[6].lt = all_true;   // All elements true
    ctx.cr[6].gt = 0;
    ctx.cr[6].eq = all_false;  // All elements false
    ctx.cr[6].so = 0;
}

//=============================================================================
// Xbox 360 Extended VMX128 Operations (non-inline implementations)
//=============================================================================

void Vmx128Unit::vmtx44mul(VectorReg vd[4], const VectorReg va[4], const VectorReg vb[4]) {
    // 4x4 matrix multiply: vd = va * vb
    // This is typically used for transform matrices in games
    VectorReg temp[4];
    
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += va[i].f32x4[k] * vb[k].f32x4[j];
            }
            temp[i].f32x4[j] = sum;
        }
    }
    
    for (int i = 0; i < 4; i++) {
        vd[i] = temp[i];
    }
}

void Vmx128Unit::vmtxtrn(VectorReg vd[4], const VectorReg va[4]) {
    // Matrix transpose
    // Uses ARM NEON for efficient transpose
    float32x4x2_t row01 = vtrnq_f32(vld1q_f32(va[0].f32x4), vld1q_f32(va[1].f32x4));
    float32x4x2_t row23 = vtrnq_f32(vld1q_f32(va[2].f32x4), vld1q_f32(va[3].f32x4));
    
    // Combine low and high halves
    vst1q_f32(vd[0].f32x4, vcombine_f32(vget_low_f32(row01.val[0]), vget_low_f32(row23.val[0])));
    vst1q_f32(vd[1].f32x4, vcombine_f32(vget_low_f32(row01.val[1]), vget_low_f32(row23.val[1])));
    vst1q_f32(vd[2].f32x4, vcombine_f32(vget_high_f32(row01.val[0]), vget_high_f32(row23.val[0])));
    vst1q_f32(vd[3].f32x4, vcombine_f32(vget_high_f32(row01.val[1]), vget_high_f32(row23.val[1])));
}

void Vmx128Unit::execute_load_store(ThreadContext& ctx, const Vmx128Inst& inst,
                                    Memory* memory, GuestAddr effective_addr) {
    VectorReg& vd = ctx.vr[inst.vd128 % cpu::NUM_VMX_REGS];
    const VectorReg& vs = ctx.vr[inst.vd128 % cpu::NUM_VMX_REGS];
    
    switch (inst.type) {
        case Vmx128Inst::Type::Lvx:
        case Vmx128Inst::Type::Lvxl:
            // Load vector (16-byte aligned)
            effective_addr &= ~15;
            memory->read_bytes(effective_addr, vd.u8x16, 16);
            break;
            
        case Vmx128Inst::Type::Stvx:
        case Vmx128Inst::Type::Stvxl:
            // Store vector (16-byte aligned)
            effective_addr &= ~15;
            memory->write_bytes(effective_addr, vs.u8x16, 16);
            break;
            
        case Vmx128Inst::Type::Lvebx:
            // Load vector element byte
            vd.u8x16[effective_addr & 15] = memory->read_u8(effective_addr);
            break;
            
        case Vmx128Inst::Type::Lvehx:
            // Load vector element halfword
            effective_addr &= ~1;
            vd.u16x8[(effective_addr >> 1) & 7] = memory->read_u16(effective_addr);
            break;
            
        case Vmx128Inst::Type::Lvewx:
            // Load vector element word
            effective_addr &= ~3;
            vd.u32x4[(effective_addr >> 2) & 3] = memory->read_u32(effective_addr);
            break;
            
        case Vmx128Inst::Type::Stvebx:
            memory->write_u8(effective_addr, vs.u8x16[effective_addr & 15]);
            break;
            
        case Vmx128Inst::Type::Stvehx:
            effective_addr &= ~1;
            memory->write_u16(effective_addr, vs.u16x8[(effective_addr >> 1) & 7]);
            break;
            
        case Vmx128Inst::Type::Stvewx:
            effective_addr &= ~3;
            memory->write_u32(effective_addr, vs.u32x4[(effective_addr >> 2) & 3]);
            break;
            
        case Vmx128Inst::Type::Lvsl:
            // Load vector for shift left - generates permute control
            {
                u8 sh = effective_addr & 15;
                for (int i = 0; i < 16; i++) {
                    vd.u8x16[i] = (sh + i) & 0x1F;
                }
            }
            break;
            
        case Vmx128Inst::Type::Lvsr:
            // Load vector for shift right
            {
                u8 sh = effective_addr & 15;
                for (int i = 0; i < 16; i++) {
                    vd.u8x16[i] = (16 - sh + i) & 0x1F;
                }
            }
            break;
            
        default:
            LOGE("Unknown VMX load/store type");
            break;
    }
}

} // namespace x360mu

