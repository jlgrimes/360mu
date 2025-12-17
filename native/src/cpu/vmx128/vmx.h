/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * VMX128 (AltiVec/Vector) instruction emulation
 * Xbox 360 extends standard AltiVec with 128 vector registers and additional instructions
 */

#pragma once

#include "x360mu/types.h"
#include "../xenon/cpu.h"
#include <arm_neon.h>

namespace x360mu {

/**
 * VMX128 instruction decoder
 */
struct Vmx128Inst {
    u32 raw;
    u8 opcode;      // Primary opcode (4 for VMX)
    u8 vd;          // Destination vector register
    u8 va;          // Source A
    u8 vb;          // Source B
    u8 vc;          // Source C (for ternary ops)
    u16 xo;         // Extended opcode
    bool rc;        // Record bit
    
    // VMX128-specific fields
    u8 vd128;       // Full 7-bit destination (for 128 regs)
    u8 va128;
    u8 vb128;
    
    enum class Type {
        Unknown,
        // Integer vector
        VAddUbm, VAddUhm, VAddUwm,  // Add unsigned
        VAddSbs, VAddShs, VAddSws,  // Add signed saturate
        VSubUbm, VSubUhm, VSubUwm,  // Subtract unsigned
        VSubSbs, VSubShs, VSubSws,  // Subtract signed saturate
        VMulEub, VMulEuh,           // Multiply even
        VMulOub, VMulOuh,           // Multiply odd
        VSum4ubs, VSum4sbs,         // Sum across
        // Float vector
        VAddFp, VSubFp, VMulFp,     // Float arithmetic
        VMaddfp, VNmsubfp,          // Float multiply-add
        VReciprocalFp,              // Reciprocal estimate
        VRsqrteFp,                  // Reciprocal sqrt estimate
        VMaxfp, VMinfp,             // Min/Max
        // Compare
        VCmpEqFp, VCmpGeFp, VCmpGtFp, VCmpBFp,
        VCmpEqub, VCmpEquh, VCmpEquw,
        VCmpGtub, VCmpGtuh, VCmpGtuw,
        VCmpGtsb, VCmpGtsh, VCmpGtsw,
        // Logical
        VAnd, VAndc, VOr, VOrc, VXor, VNor,
        // Permute/Merge
        VPerm, VPerm128,            // Permute (including 128-bit variant)
        VMrghb, VMrghh, VMrghw,     // Merge high
        VMrglb, VMrglh, VMrglw,     // Merge low
        VPkuhum, VPkuwum,           // Pack
        VUpkhsb, VUpkhsh,           // Unpack high
        VUpklsb, VUpklsh,           // Unpack low
        // Splat
        VSpltb, VSplth, VSpltw, VSpltIsb, VSpltIsh, VSpltIsw,
        // Shift/Rotate
        VSlb, VSlh, VSlw, VSld,     // Shift left
        VSrb, VSrh, VSrw, VSrd,     // Shift right logical
        VSrab, VSrah, VSraw,        // Shift right algebraic
        VRlb, VRlh, VRlw,           // Rotate left
        // Convert
        VCfux, VCfsx, VCtuxs, VCtsxs,
        VRfin, VRfiz, VRfip, VRfim,
        // Xbox 360 extensions
        VDot3fp, VDot4fp,           // Dot product
        VPack128,                   // 128-bit pack
        VUnpack128,                 // 128-bit unpack
        // Load/Store (handled separately but decoded here)
        Lvx, Lvxl, Stvx, Stvxl,
        Lvebx, Lvehx, Lvewx,
        Stvebx, Stvehx, Stvewx,
        Lvsl, Lvsr,
    } type = Type::Unknown;
};

/**
 * VMX128 execution unit
 * Handles all vector operations using ARM NEON
 */
class Vmx128Unit {
public:
    Vmx128Unit();
    
    /**
     * Decode VMX instruction
     */
    static Vmx128Inst decode(u32 inst);
    
    /**
     * Execute VMX instruction
     */
    void execute(ThreadContext& ctx, const Vmx128Inst& inst);
    
    /**
     * Execute VMX load/store (needs memory access)
     */
    void execute_load_store(ThreadContext& ctx, const Vmx128Inst& inst, 
                           class Memory* memory, GuestAddr effective_addr);
    
private:
    // NEON implementation helpers
    // Integer operations
    void vadd_ubm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vadd_uhm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vadd_uwm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsub_ubm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsub_uhm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsub_uwm(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Saturating operations
    void vaddsbs(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vaddshs(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vaddsws(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vaddubs(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vadduhs(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vadduws(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Float operations
    void vaddfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsubfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmulfp(VectorReg& vd, const VectorReg& va, const VectorReg& vc);
    void vmaddfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc);
    void vnmsubfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc);
    void vmaxfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vminfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Reciprocal/sqrt estimates
    void vrefp(VectorReg& vd, const VectorReg& vb);
    void vrsqrtefp(VectorReg& vd, const VectorReg& vb);
    
    // Dot products (Xbox 360 extension)
    void vdot3fp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vdot4fp(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Compare operations
    void vcmpeqfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    void vcmpgefp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    void vcmpgtfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    void vcmpequw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    void vcmpgtuw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    void vcmpgtsw(VectorReg& vd, const VectorReg& va, const VectorReg& vb, bool rc, ThreadContext& ctx);
    
    // Logical operations
    void vand(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vandc(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vor(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vorc(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vxor(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vnor(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Permute
    void vperm(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc);
    void vperm128(VectorReg& vd, const VectorReg& va, const VectorReg& vb, u8 perm);
    
    // Merge
    void vmrghb(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmrghh(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmrghw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmrglb(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmrglh(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vmrglw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Splat
    void vspltb(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vsplth(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vspltw(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vspltisb(VectorReg& vd, s8 simm);
    void vspltish(VectorReg& vd, s8 simm);
    void vspltisw(VectorReg& vd, s8 simm);
    
    // Shift/Rotate
    void vslb(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vslh(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vslw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsrb(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsrh(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsrw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsrab(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsrah(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vsraw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vrlb(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vrlh(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vrlw(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    
    // Conversion
    void vcfux(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vcfsx(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vctuxs(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vctsxs(VectorReg& vd, const VectorReg& vb, u8 uimm);
    void vrfin(VectorReg& vd, const VectorReg& vb);
    void vrfiz(VectorReg& vd, const VectorReg& vb);
    void vrfip(VectorReg& vd, const VectorReg& vb);
    void vrfim(VectorReg& vd, const VectorReg& vb);
    
    // Pack/Unpack
    void vpkuhum(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vpkuwum(VectorReg& vd, const VectorReg& va, const VectorReg& vb);
    void vupkhsb(VectorReg& vd, const VectorReg& vb);
    void vupkhsh(VectorReg& vd, const VectorReg& vb);
    void vupklsb(VectorReg& vd, const VectorReg& vb);
    void vupklsh(VectorReg& vd, const VectorReg& vb);
    
    // CR6 update for vector compares
    void update_cr6(ThreadContext& ctx, bool all_true, bool all_false);
};

// Inline NEON implementations for performance-critical operations

inline void Vmx128Unit::vaddfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vaddq_f32(a, b);
    vst1q_f32(vd.f32x4, result);
}

inline void Vmx128Unit::vsubfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vsubq_f32(a, b);
    vst1q_f32(vd.f32x4, result);
}

inline void Vmx128Unit::vmulfp(VectorReg& vd, const VectorReg& va, const VectorReg& vc) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t c = vld1q_f32(vc.f32x4);
    float32x4_t result = vmulq_f32(a, c);
    vst1q_f32(vd.f32x4, result);
}

inline void Vmx128Unit::vmaddfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb, const VectorReg& vc) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t c = vld1q_f32(vc.f32x4);
    // vd = (a * c) + b
    float32x4_t result = vfmaq_f32(b, a, c);
    vst1q_f32(vd.f32x4, result);
}

inline void Vmx128Unit::vmaxfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vmaxq_f32(a, b);
    vst1q_f32(vd.f32x4, result);
}

inline void Vmx128Unit::vminfp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    float32x4_t result = vminq_f32(a, b);
    vst1q_f32(vd.f32x4, result);
}

// Dot product (critical for games - physics, lighting)
inline void Vmx128Unit::vdot3fp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    
    // Multiply components
    float32x4_t prod = vmulq_f32(a, b);
    
    // Sum first 3 components (x, y, z)
    float sum = vgetq_lane_f32(prod, 0) + vgetq_lane_f32(prod, 1) + vgetq_lane_f32(prod, 2);
    
    // Broadcast to all lanes
    vd.f32x4[0] = vd.f32x4[1] = vd.f32x4[2] = vd.f32x4[3] = sum;
}

inline void Vmx128Unit::vdot4fp(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    float32x4_t a = vld1q_f32(va.f32x4);
    float32x4_t b = vld1q_f32(vb.f32x4);
    
    // Multiply all components
    float32x4_t prod = vmulq_f32(a, b);
    
    // Horizontal sum using pairwise add
    float32x2_t sum1 = vpadd_f32(vget_low_f32(prod), vget_high_f32(prod));
    float32x2_t sum2 = vpadd_f32(sum1, sum1);
    
    // Broadcast to all lanes
    float sum = vget_lane_f32(sum2, 0);
    vd.f32x4[0] = vd.f32x4[1] = vd.f32x4[2] = vd.f32x4[3] = sum;
}

inline void Vmx128Unit::vand(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vandq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

inline void Vmx128Unit::vor(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vorrq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

inline void Vmx128Unit::vxor(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = veorq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

inline void Vmx128Unit::vadd_uwm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vaddq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

inline void Vmx128Unit::vsub_uwm(VectorReg& vd, const VectorReg& va, const VectorReg& vb) {
    uint32x4_t a = vld1q_u32(va.u32x4);
    uint32x4_t b = vld1q_u32(vb.u32x4);
    uint32x4_t result = vsubq_u32(a, b);
    vst1q_u32(vd.u32x4, result);
}

} // namespace x360mu

