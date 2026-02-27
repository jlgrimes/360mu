/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos GPU Command Processor
 * Parses and executes GPU command packets from the ring buffer
 * 
 * PM4 is the packet format used by ATI/AMD GPUs (inherited from R500/Xenos)
 * 
 * Packet Header Format:
 * - Type 0: bits 30-31 = 0, bits 0-15 = base reg, bits 16-29 = count-1
 * - Type 2: bits 30-31 = 2 (NOP/padding)
 * - Type 3: bits 30-31 = 3, bits 0-7 = opcode, bits 16-29 = count
 */

#pragma once

#include "x360mu/types.h"
#include "gpu.h"
#include "gpu/vulkan/vulkan_backend.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <functional>
#include <array>

namespace x360mu {

class Memory;
class VulkanBackend;
class ShaderTranslator;
class TextureCacheImpl;
class ShaderCache;
class DescriptorManager;
class BufferPool;
struct CachedShader;
struct CachedTexture;

/**
 * GPU packet types (bits 30-31 of header)
 */
enum class PacketType : u32 {
    Type0 = 0,  // Register write
    Type1 = 1,  // Reserved (not used)
    Type2 = 2,  // NOP (padding/synchronization)
    Type3 = 3,  // Command packet
};

/**
 * PM4 Type 3 opcodes
 * Based on ATI R500/Xenos documentation
 */
enum class PM4Opcode : u32 {
    // === Basic operations ===
    NOP = 0x10,                    // No operation
    INTERRUPT = 0x40,              // Generate interrupt
    
    // === Synchronization ===
    WAIT_FOR_IDLE = 0x26,          // Wait for GPU idle
    WAIT_REG_MEM = 0x3C,           // Wait for register/memory condition
    
    // === Register operations ===
    REG_RMW = 0x21,                // Register read-modify-write
    LOAD_ALU_CONSTANT = 0x2F,      // Load ALU constants from memory
    LOAD_BOOL_CONSTANT = 0x2E,     // Load boolean constants from memory
    LOAD_LOOP_CONSTANT = 0x30,     // Load loop constants from memory
    SET_CONSTANT = 0x2D,           // Set shader constants (inline)
    SET_CONSTANT2 = 0x55,          // Set shader constants (alternate)
    SET_SHADER_CONSTANTS = 0x56,   // Set shader constants (extended)
    
    // === Drawing commands ===
    DRAW_INDX = 0x22,              // Draw indexed primitives
    DRAW_INDX_2 = 0x36,            // Draw non-indexed primitives
    DRAW_INDX_AUTO = 0x24,         // Draw with auto-generated indices
    DRAW_INDX_BIN = 0x35,          // Draw indexed with binning
    DRAW_INDX_IMMD = 0x2A,         // Draw with immediate indices
    VIZ_QUERY = 0x23,              // Visibility query
    SET_PREDICATION = 0x49,        // Set predicated rendering

    // === Memory operations ===
    MEM_WRITE = 0x3D,              // Write to memory
    COND_WRITE = 0x45,             // Conditional memory write
    EVENT_WRITE = 0x46,            // Write event (triggers actions)
    EVENT_WRITE_SHD = 0x58,        // Event write with shader data
    EVENT_WRITE_EXT = 0x59,        // Extended event write
    
    // === Binning (tiled rendering) ===
    SET_BIN_SELECT_LO = 0x60,      // Set bin selection (low)
    SET_BIN_SELECT_HI = 0x61,      // Set bin selection (high)
    SET_BIN_MASK_LO = 0x64,        // Set bin mask (low)
    SET_BIN_MASK_HI = 0x65,        // Set bin mask (high)
    
    // === Context management ===
    CONTEXT_UPDATE = 0x5E,         // Update rendering context
    
    // === Command processor control ===
    ME_INIT = 0x48,                // Initialize micro-engine
    CP_INVALIDATE_STATE = 0x3B,    // Invalidate cached state
    
    // === Indirect execution ===
    INDIRECT_BUFFER = 0x3F,        // Execute indirect command buffer
    INDIRECT_BUFFER_PFD = 0x37,    // Execute indirect buffer (pre-fetch)
    
    // === Surface operations ===
    SURFACE_SYNC = 0x43,           // Synchronize surface access
    COPY_DW = 0x4B,                // Copy dword
    COPY_DATA = 0x4C,              // Copy data block

    // === Scratch/temporary ===
    SCRATCH_RAM_WRITE = 0x4D,      // Write to scratch RAM
    SCRATCH_RAM_READ = 0x4E,       // Read from scratch RAM

    // === Shader microcode loading ===
    IM_LOAD = 0x27,                // Load shader microcode from memory
    IM_LOAD_IMMEDIATE = 0x2B,      // Load shader microcode (immediate data in packet)

    // === State invalidation ===
    INVALIDATE_STATE = 0x3B,       // Invalidate GPU state (alias for CP_INVALIDATE_STATE)
};

/**
 * Draw info extracted from command
 */
struct DrawCommand {
    PrimitiveType primitive_type;
    u32 index_count;
    GuestAddr index_base;
    u32 index_size;  // 2 or 4 bytes (sizeof index element)
    u32 vertex_count;
    bool indexed;
    u32 base_vertex;
    u32 start_index;
    u32 instance_count;  // For instanced drawing
};

/**
 * GPU state snapshot (register-derived state)
 * This represents the complete GPU state needed for rendering
 */
struct GpuState {
    // === Shader state ===
    u32 vertex_shader_addr;
    u32 pixel_shader_addr;
    
    // === Vertex format (fetch constants) ===
    u32 vertex_fetch_constants[96 * 6];  // 96 fetch constants, 6 dwords each
    
    // === Render targets ===
    u32 rb_color_info[4];   // Color buffer info for MRT
    u32 rb_depth_info;      // Depth buffer info
    u32 rb_surface_info;    // Surface dimensions
    
    // === Viewport ===
    f32 viewport_scale[4];   // x, y, z, w
    f32 viewport_offset[4];  // x, y, z, w
    
    // === Rasterizer state ===
    u32 pa_su_sc_mode_cntl;  // Cull mode, front face, etc.
    u32 pa_cl_clip_cntl;     // Clipping control
    
    // === Shader constants ===
    f32 alu_constants[256 * 4];   // 256 float4 constants
    u32 bool_constants[8];        // 256 bits (8 x 32-bit)
    u32 loop_constants[32];       // Loop iteration counts
    
    // === Texture state ===
    u32 texture_fetch_constants[32 * 6];  // 32 texture fetch constants
    
    // === Sampler state ===
    u32 sampler_state[16 * 4];  // 16 samplers, 4 dwords each
};

/**
 * Command processor handles GPU packet parsing
 * 
 * The command processor reads PM4 packets from the ring buffer,
 * decodes them, and either updates GPU state or issues draw calls
 * to the Vulkan backend.
 */
class CommandProcessor {
public:
    CommandProcessor();
    ~CommandProcessor();
    
    /**
     * Initialize with dependencies
     */
    Status initialize(Memory* memory, VulkanBackend* vulkan,
                     ShaderTranslator* shader_translator,
                     TextureCacheImpl* texture_cache,
                     ShaderCache* shader_cache = nullptr,
                     DescriptorManager* descriptor_manager = nullptr,
                     BufferPool* buffer_pool = nullptr);
    
    /**
     * Shutdown
     */
    void shutdown();
    
    /**
     * Reset state
     */
    void reset();
    
    /**
     * Process commands from ring buffer
     * Returns true if a frame was completed
     */
    bool process(GuestAddr ring_base, u32 ring_size, u32& read_ptr, u32 write_ptr);
    
    /**
     * Process a single ring buffer worth of commands
     * Used for testing - processes commands array directly
     */
    void process_ring_buffer(const u32* commands, size_t count);
    
    /**
     * Get register value
     */
    u32 get_register(u32 index) const { 
        if (index < registers_.size()) {
            return registers_[index]; 
        }
        return 0;
    }
    
    /**
     * Set register value
     */
    void set_register(u32 index, u32 value);
    
    /**
     * Write register (with side effects)
     */
    void write_register(u32 index, u32 value);
    
    /**
     * Get current GPU state snapshot
     */
    const GpuState& get_state() const { return gpu_state_; }
    
    /**
     * Get current render state (for Vulkan backend)
     */
    const RenderState& render_state() const { return render_state_; }
    
    /**
     * Frame complete flag
     */
    bool frame_complete() const { return frame_complete_; }
    void clear_frame_complete() { frame_complete_ = false; }
    
    /**
     * Statistics
     */
    u64 packets_processed() const { return packets_processed_; }
    u64 draws_this_frame() const { return draws_this_frame_; }
    u64 draws_merged() const { return draws_merged_; }
    u64 redundant_binds_skipped() const { return redundant_binds_skipped_; }

    /**
     * For testing: set a mock GPU backend
     */
    void set_vulkan_backend(VulkanBackend* vulkan) { vulkan_ = vulkan; }

private:
    // === Bound state tracking for deduplication ===
    struct BoundState {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        VkDeviceSize index_offset = 0;
        VkBuffer vertex_buffers[16] = {};
        VkDeviceSize vertex_offsets[16] = {};
        u32 vertex_buffer_count = 0;
        float viewport_x = 0, viewport_y = 0;
        float viewport_w = 0, viewport_h = 0;
        float viewport_min_z = 0, viewport_max_z = 0;
        s32 scissor_x = 0, scissor_y = 0;
        u32 scissor_w = 0, scissor_h = 0;

        void reset() { *this = BoundState{}; }
    };
    BoundState bound_state_;

    // === Draw batching ===
    struct PendingDraw {
        u32 vertex_count;
        u32 index_count;
        u32 first_vertex;
        u32 first_index;
        s32 vertex_offset;
        u32 instance_count;
        bool indexed;
    };
    static constexpr u32 kMaxBatchedDraws = 32;
    PendingDraw pending_draws_[kMaxBatchedDraws];
    u32 pending_draw_count_ = 0;
    VkPipeline batch_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet batch_descriptor_ = VK_NULL_HANDLE;

    // Batching stats
    u64 draws_merged_ = 0;
    u64 redundant_binds_skipped_ = 0;
    Memory* memory_ = nullptr;
    VulkanBackend* vulkan_ = nullptr;
    ShaderTranslator* shader_translator_ = nullptr;
    TextureCacheImpl* texture_cache_ = nullptr;
    ShaderCache* shader_cache_ = nullptr;
    DescriptorManager* descriptor_manager_ = nullptr;
    BufferPool* buffer_pool_ = nullptr;
    
    // Current frame index for descriptor management
    u32 current_frame_index_ = 0;
    
    // Cached shaders for current draw
    const CachedShader* current_vertex_shader_ = nullptr;
    const CachedShader* current_pixel_shader_ = nullptr;
    VkPipeline current_pipeline_ = VK_NULL_HANDLE;

    // Default fallback shaders (created once, used when game shaders fail)
    CachedShader* default_vertex_shader_ = nullptr;
    CachedShader* default_pixel_shader_ = nullptr;
    
    // GPU registers (complete register file)
    std::array<u32, 0x10000> registers_;
    
    // Current GPU state (derived from registers)
    GpuState gpu_state_;
    
    // Current render state (for Vulkan backend)
    RenderState render_state_;
    
    // Shader constants
    std::array<f32, 256 * 4> vertex_constants_;   // 256 float4 constants
    std::array<f32, 256 * 4> pixel_constants_;
    std::array<u32, 256> bool_constants_;
    std::array<u32, 32> loop_constants_;

    // Dirty flags for constant upload optimization
    bool vertex_constants_dirty_ = true;
    bool pixel_constants_dirty_ = true;
    bool bool_constants_dirty_ = true;
    bool loop_constants_dirty_ = true;

    // Fetch constants (vertex buffers + textures)
    std::array<FetchConstant, 96> vertex_fetch_;
    std::array<FetchConstant, 32> texture_fetch_;
    
    // Frame state
    bool frame_complete_ = false;
    bool in_frame_ = false;
    
    // Stats
    u64 packets_processed_ = 0;
    u64 draws_this_frame_ = 0;
    
    // For direct buffer processing (testing)
    const u32* direct_buffer_ = nullptr;
    size_t direct_buffer_size_ = 0;
    size_t direct_buffer_pos_ = 0;

    // Active packet stream context for wrapped ring-buffer payload reads.
    // Non-zero only while executing a packet sourced from a circular ring.
    GuestAddr stream_base_ = 0;
    u32 stream_size_bytes_ = 0;

    // Indirect buffer recursion guard
    u32 ib_depth_ = 0;
    static constexpr u32 kMaxIBDepth = 4;

    // Scratch RAM (256 dwords, used by CP microcode)
    std::array<u32, 256> scratch_ram_{};

    // Binning state
    u32 bin_mask_lo_ = 0xFFFFFFFF;
    u32 bin_mask_hi_ = 0xFFFFFFFF;
    u32 bin_select_lo_ = 0;
    u32 bin_select_hi_ = 0;

    // Occlusion query state
    static constexpr u32 MAX_OCCLUSION_QUERIES = 256;
    struct OcclusionQueryState {
        bool active = false;
        u32 active_query_id = 0;
        u32 next_query_id = 0;
        bool query_pool_initialized = false;
        std::array<u32, 256> guest_to_vk_query{};
        std::array<u64, 256> query_results{};
        std::array<bool, 256> query_valid{};
    };
    OcclusionQueryState occlusion_;

    // Predication state
    struct PredicationState {
        bool active = false;
        u32 query_index = 0;
        bool inverted = false;
        bool wait = false;
        bool use_hw_predication = false;
    };
    PredicationState predication_;

    // Shader microcode staging (for IM_LOAD)
    struct ShaderMicrocodeSlot {
        std::vector<u32> data;
        ShaderType type = ShaderType::Vertex;
        u32 start_offset = 0;
    };
    ShaderMicrocodeSlot pending_shader_;
    
    // Packet processing
    // stream_base/stream_size_bytes are optional ring-stream context used to
    // wrap packet payload reads when a packet straddles the end of the ring.
    u32 execute_packet(GuestAddr addr, u32& packets_consumed,
                       GuestAddr stream_base = 0, u32 stream_size_bytes = 0);
    u32 execute_packet_direct(const u32* packet, u32& packets_consumed);
    void execute_type0(u32 header, GuestAddr data_addr);
    void execute_type0_direct(u32 header, const u32* data);
    void execute_type2(u32 header);
    void execute_type3(u32 header, GuestAddr data_addr);
    void execute_type3_direct(u32 header, const u32* data);
    
    // Type 3 handlers (memory-based)
    void handle_draw_indx(GuestAddr data_addr, u32 count);
    void handle_draw_indx_2(GuestAddr data_addr, u32 count);
    void handle_draw_indx_auto(GuestAddr data_addr, u32 count);
    void handle_draw_indx_immd(GuestAddr data_addr, u32 count);
    void handle_load_alu_constant(GuestAddr data_addr, u32 count);
    void handle_load_bool_constant(GuestAddr data_addr, u32 count);
    void handle_load_loop_constant(GuestAddr data_addr, u32 count);
    void handle_set_constant(GuestAddr data_addr, u32 count);
    void handle_event_write(GuestAddr data_addr, u32 count);
    void handle_mem_write(GuestAddr data_addr, u32 count);
    void handle_wait_reg_mem(GuestAddr data_addr, u32 count);
    void handle_indirect_buffer(GuestAddr data_addr, u32 count);
    void handle_cond_write(GuestAddr data_addr, u32 count);
    void handle_surface_sync(GuestAddr data_addr, u32 count);
    void handle_event_write_shd(GuestAddr data_addr, u32 count);
    void handle_im_load(GuestAddr data_addr, u32 count);
    void handle_im_load_immediate(GuestAddr data_addr, u32 count);
    void handle_draw_indx_bin(GuestAddr data_addr, u32 count);
    void handle_copy_dw(GuestAddr data_addr, u32 count);
    void handle_viz_query(GuestAddr data_addr, u32 count);
    void handle_set_predication(GuestAddr data_addr, u32 count);
    void handle_set_bin_mask(GuestAddr data_addr, u32 count, bool hi);
    void handle_set_bin_select(GuestAddr data_addr, u32 count, bool hi);

    // Type 3 handlers (direct buffer for testing)
    void handle_draw_indx_direct(const u32* data, u32 count);
    void handle_draw_indx_auto_direct(const u32* data, u32 count);
    void handle_set_constant_direct(const u32* data, u32 count);
    
    // State update
    void update_render_state();
    void update_shaders();
    void update_textures();
    void update_vertex_buffers();
    void update_gpu_state();
    
    // Draw execution
    void execute_draw(const DrawCommand& cmd);
    
    // Shader and pipeline management
    bool prepare_shaders();
    bool prepare_pipeline(const DrawCommand& cmd);
    void set_dynamic_state();
    void bind_vertex_buffers(const DrawCommand& cmd);
    void bind_index_buffer(const DrawCommand& cmd);
    void build_vertex_input_state(VertexInputConfig& config);
    void update_constants();
    void bind_textures();

    // State deduplication - skip redundant Vulkan binds
    bool bind_pipeline_dedup(VkPipeline pipeline);
    bool bind_descriptor_set_dedup(VkDescriptorSet set);
    void set_viewport_dedup(float x, float y, float w, float h, float minz, float maxz);
    void set_scissor_dedup(s32 x, s32 y, u32 w, u32 h);
    void bind_vertex_buffers_dedup(u32 count, const VkBuffer* buffers, const VkDeviceSize* offsets);
    void bind_index_buffer_dedup(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
    void reset_bound_state();

    // Draw batching
    void queue_draw(const DrawCommand& cmd);
    void flush_draw_batch();
    bool can_merge_draw(const DrawCommand& cmd) const;

    // Default shader management
    void create_default_shaders();
    void use_default_shaders();
    void cleanup_default_shaders();
    
    // Tessellation and primitive expansion
    bool needs_tessellation(const DrawCommand& cmd) const;
    DrawCommand tessellate_draw(const DrawCommand& cmd);
    void tessellate_tri_patch(std::vector<f32>& out_vertices, u32 tess_level);
    void tessellate_quad_patch(std::vector<f32>& out_vertices, u32 tess_level);
    DrawCommand expand_rect_list(const DrawCommand& cmd);

    // Register side effects
    void on_register_write(u32 index, u32 value);
    
    // Type 0 register write helpers
    void process_type0_write(u32 base_reg, const u32* data, u32 count);
    
    // Helper to read command buffer
    u32 read_cmd(GuestAddr addr);
};

// ShaderMicrocode is defined in shader_translator.h

} // namespace x360mu

