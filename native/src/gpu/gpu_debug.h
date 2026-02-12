/**
 * 360mu - Xbox 360 Emulator for Android
 *
 * GPU Debug Validation and Tracing
 *
 * Provides draw call tracing, frame capture, validation checks,
 * and per-frame statistics for GPU debugging.
 *
 * Enable at compile time: -DGPU_DEBUG_ENABLED=1
 * Enable at runtime:      GpuDebugTracer::instance().set_enabled(true)
 */

#pragma once

#include "x360mu/types.h"
#include "xenos/gpu.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define GPU_DBG_TAG "360mu-gpudbg"
#define GPU_DBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, GPU_DBG_TAG, __VA_ARGS__)
#define GPU_DBG_WARN(...) __android_log_print(ANDROID_LOG_WARN, GPU_DBG_TAG, __VA_ARGS__)
#else
#define GPU_DBG_LOG(...) do { printf("[GPU-DBG] " __VA_ARGS__); printf("\n"); } while(0)
#define GPU_DBG_WARN(...) do { fprintf(stderr, "[GPU-DBG WARN] " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

namespace x360mu {

// ============================================================================
// Per-draw-call trace record
// ============================================================================

struct DrawTrace {
    u64 draw_index;

    // Draw parameters
    u32 vertex_count;
    u32 index_count;
    u32 instance_count;
    u32 primitive_type;
    bool indexed;
    GuestAddr index_base;
    u32 index_size;

    // Shader state
    GuestAddr vs_addr;
    GuestAddr ps_addr;
    u64 vs_hash;
    u64 ps_hash;
    bool using_default_shaders;

    // Pipeline state
    VkPrimitiveTopology topology;
    VkCullModeFlags cull_mode;
    VkBool32 depth_test;
    VkBool32 depth_write;
    VkBool32 blend_enable;
    VkPipeline pipeline;

    // Texture binds
    u32 texture_count;
    struct TextureBind {
        u32 slot;
        GuestAddr address;
        u32 width;
        u32 height;
        u32 format;
    };
    TextureBind textures[16];

    // Validation results
    bool valid_shaders;
    bool valid_pipeline;
    bool valid_render_pass;
    bool valid_descriptors;
};

struct RegisterWriteTrace {
    u32 index;
    u32 old_value;
    u32 new_value;
};

struct ShaderCompileTrace {
    GuestAddr address;
    ShaderType type;
    u64 hash;
    bool success;
    bool used_fallback;
    u32 spirv_word_count;
};

// ============================================================================
// Per-frame statistics
// ============================================================================

struct GpuFrameStats {
    u64 frame_index;
    u32 draw_calls;
    u32 draw_calls_skipped;
    u64 primitives;
    u32 shader_compiles;
    u32 shader_cache_hits;
    u32 shader_fallbacks;
    u32 pipeline_creates;
    u32 pipeline_cache_hits;
    u32 texture_binds;
    u32 register_writes;
    u32 pm4_packets;
    u32 validation_warnings;
};

// ============================================================================
// Frame capture (for JSON export)
// ============================================================================

struct FrameCapture {
    GpuFrameStats stats;
    std::vector<DrawTrace> draws;
    std::vector<RegisterWriteTrace> critical_reg_writes;
    std::vector<ShaderCompileTrace> shader_compiles;
};

// ============================================================================
// GPU Debug Tracer (singleton)
// ============================================================================

class GpuDebugTracer {
public:
    static GpuDebugTracer& instance() {
        static GpuDebugTracer s_instance;
        return s_instance;
    }

    // --- Enable/disable ---
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const {
#ifdef GPU_DEBUG_ENABLED
        return true;
#else
        return enabled_;
#endif
    }

    // --- Frame lifecycle ---
    void begin_frame(u64 frame_index) {
        if (!enabled()) return;
        current_stats_ = {};
        current_stats_.frame_index = frame_index;
        if (capturing_) {
            capture_.stats = {};
            capture_.stats.frame_index = frame_index;
            capture_.draws.clear();
            capture_.critical_reg_writes.clear();
            capture_.shader_compiles.clear();
        }
    }

    void end_frame() {
        if (!enabled()) return;
        GPU_DBG_LOG("Frame %llu: %u draws, %llu prims, %u shaders(%u fallback), %u pipelines, %u warnings",
                    (unsigned long long)current_stats_.frame_index,
                    current_stats_.draw_calls,
                    (unsigned long long)current_stats_.primitives,
                    current_stats_.shader_compiles,
                    current_stats_.shader_fallbacks,
                    current_stats_.pipeline_creates,
                    current_stats_.validation_warnings);
        if (capturing_) {
            capture_.stats = current_stats_;
            capturing_ = false;
            save_capture();
        }
        total_frames_++;
    }

    // --- PM4 packet tracing ---
    void trace_pm4_packet(u32 type, u32 opcode, u32 count) {
        if (!enabled()) return;
        current_stats_.pm4_packets++;
        GPU_DBG_LOG("PM4: type=%u opcode=0x%02X count=%u", type, opcode, count);
    }

    // --- Register write tracing ---
    void trace_register_write(u32 index, u32 old_val, u32 new_val) {
        if (!enabled()) return;
        current_stats_.register_writes++;
        // Only log rendering-critical registers
        if (is_critical_register(index)) {
            GPU_DBG_LOG("REG[0x%04X]: 0x%08X -> 0x%08X (%s)",
                        index, old_val, new_val, register_name(index));
            if (capturing_) {
                capture_.critical_reg_writes.push_back({index, old_val, new_val});
            }
        }
    }

    // --- Shader compilation tracing ---
    void trace_shader_compile(GuestAddr addr, ShaderType type, u64 hash,
                              bool success, bool fallback, u32 spirv_words) {
        if (!enabled()) return;
        current_stats_.shader_compiles++;
        if (fallback) current_stats_.shader_fallbacks++;
        GPU_DBG_LOG("SHADER %s: addr=%08x hash=%016llx %s%s spirv=%u words",
                    type == ShaderType::Vertex ? "VS" : "PS",
                    addr, (unsigned long long)hash,
                    success ? "OK" : "FAIL",
                    fallback ? " (FALLBACK)" : "",
                    spirv_words);
        if (capturing_) {
            capture_.shader_compiles.push_back({addr, type, hash, success, fallback, spirv_words});
        }
    }

    void trace_shader_cache_hit() {
        if (!enabled()) return;
        current_stats_.shader_cache_hits++;
    }

    // --- Pipeline tracing ---
    void trace_pipeline_create(u64 vs_hash, u64 ps_hash, VkPrimitiveTopology topo,
                               VkPipeline pipeline) {
        if (!enabled()) return;
        current_stats_.pipeline_creates++;
        GPU_DBG_LOG("PIPELINE: vs=%016llx ps=%016llx topo=%u -> %p",
                    (unsigned long long)vs_hash, (unsigned long long)ps_hash,
                    (u32)topo, (void*)pipeline);
    }

    void trace_pipeline_cache_hit() {
        if (!enabled()) return;
        current_stats_.pipeline_cache_hits++;
    }

    // --- Draw call tracing ---
    void trace_draw(const DrawTrace& draw) {
        if (!enabled()) return;
        current_stats_.draw_calls++;
        u32 prim_count = draw.indexed ? draw.index_count / 3 : draw.vertex_count / 3;
        current_stats_.primitives += prim_count;

        GPU_DBG_LOG("DRAW #%llu: %s %u %s, prim=%u, vs=%08x ps=%08x %s pipe=%p",
                    (unsigned long long)draw.draw_index,
                    draw.indexed ? "indexed" : "non-idx",
                    draw.indexed ? draw.index_count : draw.vertex_count,
                    draw.indexed ? "indices" : "verts",
                    draw.primitive_type,
                    draw.vs_addr, draw.ps_addr,
                    draw.using_default_shaders ? "(DEFAULT)" : "",
                    (void*)draw.pipeline);

        if (draw.texture_count > 0) {
            for (u32 i = 0; i < draw.texture_count; i++) {
                const auto& t = draw.textures[i];
                GPU_DBG_LOG("  TEX[%u]: addr=%08x %ux%u fmt=%u",
                            t.slot, t.address, t.width, t.height, t.format);
            }
            current_stats_.texture_binds += draw.texture_count;
        }

        if (capturing_) {
            capture_.draws.push_back(draw);
        }
    }

    void trace_draw_skipped(const char* reason) {
        if (!enabled()) return;
        current_stats_.draw_calls_skipped++;
        GPU_DBG_LOG("DRAW SKIPPED: %s", reason);
    }

    // --- Validation checks ---
    bool validate_draw(const DrawTrace& draw) {
        if (!enabled()) return true;
        bool ok = true;

        if (!draw.valid_shaders) {
            GPU_DBG_WARN("VALIDATION: draw #%llu has null shader modules",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }
        if (!draw.valid_pipeline) {
            GPU_DBG_WARN("VALIDATION: draw #%llu has null pipeline",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }
        if (!draw.valid_render_pass) {
            GPU_DBG_WARN("VALIDATION: draw #%llu issued outside render pass",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }
        if (!draw.valid_descriptors) {
            GPU_DBG_WARN("VALIDATION: draw #%llu has no descriptor set bound",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }
        if (draw.indexed && draw.index_count == 0) {
            GPU_DBG_WARN("VALIDATION: draw #%llu indexed with 0 indices",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }
        if (!draw.indexed && draw.vertex_count == 0) {
            GPU_DBG_WARN("VALIDATION: draw #%llu with 0 vertices",
                         (unsigned long long)draw.draw_index);
            current_stats_.validation_warnings++;
            ok = false;
        }

        return ok;
    }

    // --- Frame capture ---
    void start_capture() { capturing_ = true; }
    bool is_capturing() const { return capturing_; }
    const FrameCapture& last_capture() const { return capture_; }

    // --- Stats access ---
    const GpuFrameStats& current_frame_stats() const { return current_stats_; }
    u64 total_frames() const { return total_frames_; }

private:
    GpuDebugTracer() = default;

    bool enabled_ = false;
    bool capturing_ = false;
    u64 total_frames_ = 0;

    GpuFrameStats current_stats_{};
    FrameCapture capture_;

    void save_capture() {
        char path[256];
        snprintf(path, sizeof(path), "/tmp/360mu_frame_%llu.json",
                 (unsigned long long)capture_.stats.frame_index);

        FILE* f = fopen(path, "w");
        if (!f) {
            GPU_DBG_WARN("Failed to save frame capture to %s", path);
            return;
        }

        fprintf(f, "{\n");
        fprintf(f, "  \"frame\": %llu,\n", (unsigned long long)capture_.stats.frame_index);
        fprintf(f, "  \"draw_calls\": %u,\n", capture_.stats.draw_calls);
        fprintf(f, "  \"draw_calls_skipped\": %u,\n", capture_.stats.draw_calls_skipped);
        fprintf(f, "  \"primitives\": %llu,\n", (unsigned long long)capture_.stats.primitives);
        fprintf(f, "  \"shader_compiles\": %u,\n", capture_.stats.shader_compiles);
        fprintf(f, "  \"shader_fallbacks\": %u,\n", capture_.stats.shader_fallbacks);
        fprintf(f, "  \"pipeline_creates\": %u,\n", capture_.stats.pipeline_creates);
        fprintf(f, "  \"validation_warnings\": %u,\n", capture_.stats.validation_warnings);

        // Draws
        fprintf(f, "  \"draws\": [\n");
        for (size_t i = 0; i < capture_.draws.size(); i++) {
            const auto& d = capture_.draws[i];
            fprintf(f, "    {\"index\": %llu, \"indexed\": %s, \"count\": %u, "
                       "\"prim_type\": %u, \"vs_addr\": \"0x%08x\", \"ps_addr\": \"0x%08x\", "
                       "\"vs_hash\": \"%016llx\", \"ps_hash\": \"%016llx\", "
                       "\"default_shaders\": %s, \"textures\": %u, "
                       "\"valid\": %s}%s\n",
                    (unsigned long long)d.draw_index,
                    d.indexed ? "true" : "false",
                    d.indexed ? d.index_count : d.vertex_count,
                    d.primitive_type,
                    d.vs_addr, d.ps_addr,
                    (unsigned long long)d.vs_hash, (unsigned long long)d.ps_hash,
                    d.using_default_shaders ? "true" : "false",
                    d.texture_count,
                    (d.valid_shaders && d.valid_pipeline) ? "true" : "false",
                    (i + 1 < capture_.draws.size()) ? "," : "");
        }
        fprintf(f, "  ],\n");

        // Shader compiles
        fprintf(f, "  \"shader_compiles\": [\n");
        for (size_t i = 0; i < capture_.shader_compiles.size(); i++) {
            const auto& s = capture_.shader_compiles[i];
            fprintf(f, "    {\"addr\": \"0x%08x\", \"type\": \"%s\", "
                       "\"hash\": \"%016llx\", \"success\": %s, "
                       "\"fallback\": %s, \"spirv_words\": %u}%s\n",
                    s.address,
                    s.type == ShaderType::Vertex ? "VS" : "PS",
                    (unsigned long long)s.hash,
                    s.success ? "true" : "false",
                    s.used_fallback ? "true" : "false",
                    s.spirv_word_count,
                    (i + 1 < capture_.shader_compiles.size()) ? "," : "");
        }
        fprintf(f, "  ],\n");

        // Critical register writes
        fprintf(f, "  \"register_writes\": [\n");
        for (size_t i = 0; i < capture_.critical_reg_writes.size(); i++) {
            const auto& r = capture_.critical_reg_writes[i];
            fprintf(f, "    {\"reg\": \"0x%04X\", \"name\": \"%s\", "
                       "\"old\": \"0x%08X\", \"new\": \"0x%08X\"}%s\n",
                    r.index, register_name(r.index),
                    r.old_value, r.new_value,
                    (i + 1 < capture_.critical_reg_writes.size()) ? "," : "");
        }
        fprintf(f, "  ]\n");

        fprintf(f, "}\n");
        fclose(f);

        GPU_DBG_LOG("Frame capture saved to %s (%zu draws, %zu shader compiles)",
                    path, capture_.draws.size(), capture_.shader_compiles.size());
    }

    static bool is_critical_register(u32 index) {
        // Only trace registers that affect rendering
        return (index == xenos_reg::SQ_VS_PROGRAM ||
                index == xenos_reg::SQ_PS_PROGRAM ||
                index == xenos_reg::SQ_PROGRAM_CNTL ||
                index == xenos_reg::RB_COLOR_INFO ||
                index == xenos_reg::RB_DEPTH_INFO ||
                index == xenos_reg::RB_SURFACE_INFO ||
                index == xenos_reg::RB_DEPTHCONTROL ||
                index == xenos_reg::RB_BLENDCONTROL ||
                index == xenos_reg::RB_MODECONTROL ||
                index == xenos_reg::PA_CL_VTE_CNTL ||
                index == xenos_reg::PA_SU_SC_MODE_CNTL ||
                index == xenos_reg::PA_CL_CLIP_CNTL ||
                index == xenos_reg::VGT_DRAW_INITIATOR);
    }

    static const char* register_name(u32 index) {
        switch (index) {
            case xenos_reg::SQ_VS_PROGRAM: return "SQ_VS_PROGRAM";
            case xenos_reg::SQ_PS_PROGRAM: return "SQ_PS_PROGRAM";
            case xenos_reg::SQ_PROGRAM_CNTL: return "SQ_PROGRAM_CNTL";
            case xenos_reg::RB_COLOR_INFO: return "RB_COLOR_INFO";
            case xenos_reg::RB_DEPTH_INFO: return "RB_DEPTH_INFO";
            case xenos_reg::RB_SURFACE_INFO: return "RB_SURFACE_INFO";
            case xenos_reg::RB_DEPTHCONTROL: return "RB_DEPTHCONTROL";
            case xenos_reg::RB_BLENDCONTROL: return "RB_BLENDCONTROL";
            case xenos_reg::RB_MODECONTROL: return "RB_MODECONTROL";
            case xenos_reg::PA_CL_VTE_CNTL: return "PA_CL_VTE_CNTL";
            case xenos_reg::PA_SU_SC_MODE_CNTL: return "PA_SU_SC_MODE_CNTL";
            case xenos_reg::PA_CL_CLIP_CNTL: return "PA_CL_CLIP_CNTL";
            case xenos_reg::VGT_DRAW_INITIATOR: return "VGT_DRAW_INITIATOR";
            default: return "UNKNOWN";
        }
    }
};

// ============================================================================
// Convenience macros (compile out when GPU_DEBUG_ENABLED is not set)
// ============================================================================

#ifdef GPU_DEBUG_ENABLED
#define GPU_TRACE_PM4(type, op, cnt) GpuDebugTracer::instance().trace_pm4_packet(type, op, cnt)
#define GPU_TRACE_REG(idx, old, nw)  GpuDebugTracer::instance().trace_register_write(idx, old, nw)
#define GPU_TRACE_DRAW(d)            GpuDebugTracer::instance().trace_draw(d)
#define GPU_TRACE_DRAW_SKIP(r)       GpuDebugTracer::instance().trace_draw_skipped(r)
#define GPU_VALIDATE_DRAW(d)         GpuDebugTracer::instance().validate_draw(d)
#define GPU_TRACE_SHADER(a,t,h,s,f,w) GpuDebugTracer::instance().trace_shader_compile(a,t,h,s,f,w)
#define GPU_TRACE_PIPELINE(vh,ph,t,p)  GpuDebugTracer::instance().trace_pipeline_create(vh,ph,t,p)
#define GPU_BEGIN_FRAME(idx)         GpuDebugTracer::instance().begin_frame(idx)
#define GPU_END_FRAME()              GpuDebugTracer::instance().end_frame()
#else
// Runtime-checked versions (slightly more overhead but work with runtime toggle)
#define GPU_TRACE_PM4(type, op, cnt) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_pm4_packet(type, op, cnt); } while(0)
#define GPU_TRACE_REG(idx, old, nw) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_register_write(idx, old, nw); } while(0)
#define GPU_TRACE_DRAW(d) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_draw(d); } while(0)
#define GPU_TRACE_DRAW_SKIP(r) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_draw_skipped(r); } while(0)
#define GPU_VALIDATE_DRAW(d) \
    (GpuDebugTracer::instance().enabled() ? GpuDebugTracer::instance().validate_draw(d) : true)
#define GPU_TRACE_SHADER(a,t,h,s,f,w) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_shader_compile(a,t,h,s,f,w); } while(0)
#define GPU_TRACE_PIPELINE(vh,ph,t,p) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().trace_pipeline_create(vh,ph,t,p); } while(0)
#define GPU_BEGIN_FRAME(idx) \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().begin_frame(idx); } while(0)
#define GPU_END_FRAME() \
    do { if (GpuDebugTracer::instance().enabled()) GpuDebugTracer::instance().end_frame(); } while(0)
#endif

} // namespace x360mu
