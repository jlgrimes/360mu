/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Stub GPU implementation when Vulkan is not available
 * Provides enough functionality for tests to link and pass
 */

// Include full headers for complete type definitions
#include "gpu/xenos/shader_translator.h"
#include "gpu/xenos/texture.h"
#include "gpu/xenos/command_processor.h"
#include "gpu/default_shaders.h"

// Define empty types for classes only used via unique_ptr in Gpu
// (real implementations live in .cpp files we don't link)
namespace x360mu {
    class ShaderCache {};
    class DescriptorManager {};
    class BufferPool {};
    class RenderTargetManager {};
    class EdramManager {};
    class TextureCacheImpl {};
}

#include "gpu/xenos/gpu.h"
#include "memory/memory.h"

// ===========================================================================
// Inline PM4 parsing helpers (matching command_processor.cpp)
// ===========================================================================
namespace {
using namespace x360mu;

inline PacketType get_packet_type(u32 header) {
    return static_cast<PacketType>((header >> 30) & 0x3);
}
inline u32 type0_base_index(u32 header) { return header & 0x7FFF; }
inline u32 type0_count(u32 header) { return ((header >> 16) & 0x3FFF) + 1; }
inline u32 type3_count(u32 header) { return (header >> 16) & 0x3FFF; }

} // anonymous namespace

namespace x360mu {

// ===========================================================================
// VulkanBackend stubs (needed for unique_ptr<VulkanBackend> in Gpu)
// ===========================================================================
VulkanBackend::VulkanBackend() = default;
VulkanBackend::~VulkanBackend() = default;

Status VulkanBackend::initialize(void*, u32, u32) { return Status::Ok; }
void VulkanBackend::shutdown() {}

// ===========================================================================
// Gpu implementation
// ===========================================================================
Gpu::Gpu() = default;
Gpu::~Gpu() = default;

Status Gpu::initialize(Memory* memory, const GpuConfig& config) {
    memory_ = memory;
    config_ = config;
    registers_.fill(0);
    registers_[0x0010] = 0x80000000; // GRBM_STATUS: idle
    return Status::Ok;
}

void Gpu::shutdown() {}

void Gpu::reset() {
    registers_.fill(0);
    registers_[0x0010] = 0x80000000; // GRBM_STATUS: idle
    render_state_ = {};
    frame_complete_ = false;
    in_frame_ = false;
    stats_ = {};
}

void Gpu::set_surface(void*) {}
void Gpu::resize(u32, u32) {}
void Gpu::process_commands() {}

void Gpu::present() {
    frame_complete_ = true;
    stats_.frames++;
}

u32 Gpu::read_register(u32 offset) {
    if (offset < registers_.size()) {
        return registers_[offset];
    }
    return 0;
}

void Gpu::write_register(u32 offset, u32 value) {
    if (offset < registers_.size()) {
        registers_[offset] = value;
    }
}

void Gpu::execute_packet(u32) {}
void Gpu::execute_type0(u32) {}
void Gpu::execute_type3(u32) {}
void Gpu::cmd_draw_indices(PrimitiveType, u32, GuestAddr) {}
void Gpu::cmd_draw_auto(PrimitiveType, u32) {}
void Gpu::cmd_resolve() {}
void Gpu::update_render_state() {}
void Gpu::update_render_targets() {}
void Gpu::update_shaders() {}
void Gpu::update_textures() {}
void Gpu::test_render() {}
void Gpu::set_title_id(u32) {}
void Gpu::signal_vsync() {}
void Gpu::set_vsync(bool enabled) { config_.enable_vsync = enabled; }
void Gpu::set_frame_skip(u32 count) { frame_skip_ = count; }
void Gpu::set_target_fps(u32 fps) { target_fps_ = fps; }

void Gpu::cpu_signal_fence(u64 value) { cpu_fence_.store(value, std::memory_order_release); }
void Gpu::gpu_signal_fence(u64 value) {
    gpu_fence_.store(value, std::memory_order_release);
    fence_cv_.notify_all();
}
bool Gpu::wait_for_gpu_fence(u64, u64) { return true; }

// ===========================================================================
// ShaderTranslator stub
// ===========================================================================
ShaderTranslator::ShaderTranslator() = default;
ShaderTranslator::~ShaderTranslator() = default;

Status ShaderTranslator::initialize(const std::string&) { return Status::Ok; }
void ShaderTranslator::shutdown() {}

std::vector<u32> ShaderTranslator::translate(const void*, u32, ShaderType) { return {}; }

const std::vector<u32>* ShaderTranslator::get_cached(u64 hash) const {
    auto it = cache_.find(hash);
    if (it != cache_.end()) return &it->second;
    return nullptr;
}

void ShaderTranslator::cache(u64 hash, std::vector<u32> spirv) {
    cache_[hash] = std::move(spirv);
}

void ShaderTranslator::save_cache() {}
void ShaderTranslator::load_cache() {}

ShaderInfo ShaderTranslator::analyze(const void*, u32, ShaderType) { return ShaderInfo{}; }
u64 ShaderTranslator::compute_hash(const void* data, u32 size) {
    // FNV-1a hash
    u64 hash = 14695981039346656037ULL;
    auto bytes = static_cast<const u8*>(data);
    for (u32 i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ===========================================================================
// TextureCache stub
// ===========================================================================
TextureCache::TextureCache() = default;
TextureCache::~TextureCache() = default;

Status TextureCache::initialize(u32) { return Status::Ok; }
void TextureCache::shutdown() {}
const u8* TextureCache::get_texture(const TextureInfo&, class Memory*) { return nullptr; }
void TextureCache::invalidate_range(GuestAddr, u32) {}
void TextureCache::invalidate_all() {}
TextureCache::Stats TextureCache::get_stats() const { return Stats{}; }

// ===========================================================================
// CommandProcessor stub - with real PM4 Type 0 parsing for tests
// ===========================================================================

CommandProcessor::CommandProcessor() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
    gpu_state_ = {};
    render_state_ = {};
    frame_complete_ = false;
    in_frame_ = false;
    packets_processed_ = 0;
    draws_this_frame_ = 0;
    draws_merged_ = 0;
    redundant_binds_skipped_ = 0;
    direct_buffer_ = nullptr;
    direct_buffer_size_ = 0;
    direct_buffer_pos_ = 0;
    ib_depth_ = 0;
    scratch_ram_.fill(0);
    bin_mask_lo_ = 0xFFFFFFFF;
    bin_mask_hi_ = 0xFFFFFFFF;
    bin_select_lo_ = 0;
    bin_select_hi_ = 0;
    pending_shader_ = {};
    current_vertex_shader_ = nullptr;
    current_pixel_shader_ = nullptr;
    current_pipeline_ = VK_NULL_HANDLE;
    default_vertex_shader_ = nullptr;
    default_pixel_shader_ = nullptr;
    bound_state_ = {};
    pending_draw_count_ = 0;
    batch_pipeline_ = VK_NULL_HANDLE;
    batch_descriptor_ = VK_NULL_HANDLE;
    current_frame_index_ = 0;
}

CommandProcessor::~CommandProcessor() {
    shutdown();
}

Status CommandProcessor::initialize(Memory* memory, VulkanBackend* vulkan,
                                    ShaderTranslator* shader_translator,
                                    TextureCacheImpl* texture_cache,
                                    ShaderCache* shader_cache,
                                    DescriptorManager* descriptor_manager,
                                    BufferPool* buffer_pool,
                                    EdramManager*) {
    memory_ = memory;
    vulkan_ = vulkan;
    shader_translator_ = shader_translator;
    texture_cache_ = texture_cache;
    shader_cache_ = shader_cache;
    descriptor_manager_ = descriptor_manager;
    buffer_pool_ = buffer_pool;
    reset();
    return Status::Ok;
}

void CommandProcessor::shutdown() {
    memory_ = nullptr;
    vulkan_ = nullptr;
    shader_translator_ = nullptr;
    texture_cache_ = nullptr;
    shader_cache_ = nullptr;
    descriptor_manager_ = nullptr;
    buffer_pool_ = nullptr;
    current_vertex_shader_ = nullptr;
    current_pixel_shader_ = nullptr;
    current_pipeline_ = VK_NULL_HANDLE;
    default_vertex_shader_ = nullptr;
    default_pixel_shader_ = nullptr;
}

void CommandProcessor::reset() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
    vertex_constants_dirty_ = true;
    pixel_constants_dirty_ = true;
    bool_constants_dirty_ = true;
    loop_constants_dirty_ = true;
    gpu_state_ = {};
    render_state_ = {};
    frame_complete_ = false;
    in_frame_ = false;
    packets_processed_ = 0;
    draws_this_frame_ = 0;
    direct_buffer_ = nullptr;
    direct_buffer_size_ = 0;
    direct_buffer_pos_ = 0;
    ib_depth_ = 0;
    scratch_ram_.fill(0);
    bin_mask_lo_ = 0xFFFFFFFF;
    bin_mask_hi_ = 0xFFFFFFFF;
    bin_select_lo_ = 0;
    bin_select_hi_ = 0;
    pending_shader_ = {};
}

void CommandProcessor::set_register(u32 index, u32 value) {
    if (index < registers_.size()) {
        registers_[index] = value;
    }
}

void CommandProcessor::write_register(u32 index, u32 value) {
    set_register(index, value);
    on_register_write(index, value);
}

u32 CommandProcessor::read_cmd(GuestAddr addr) {
    if (memory_) return memory_->read_u32(addr);
    return 0;
}

// --- Packet processing (memory-based, used by process()) ---

bool CommandProcessor::process(GuestAddr ring_base, u32 ring_size, u32& read_ptr, u32 write_ptr) {
    frame_complete_ = false;
    u32 safety = 0;
    constexpr u32 kMaxPackets = 100000;

    while (read_ptr != write_ptr && safety++ < kMaxPackets) {
        GuestAddr packet_addr = ring_base + (read_ptr * 4);
        u32 packets_consumed = 0;
        execute_packet(packet_addr, packets_consumed);

        if (packets_consumed == 0) packets_consumed = 1;
        read_ptr = (read_ptr + packets_consumed) % ring_size;
        packets_processed_++;

        if (frame_complete_) break;
    }
    return frame_complete_;
}

u32 CommandProcessor::execute_packet(GuestAddr addr, u32& packets_consumed) {
    u32 header = read_cmd(addr);
    PacketType type = get_packet_type(header);

    switch (type) {
        case PacketType::Type0:
            execute_type0(header, addr + 4);
            packets_consumed = 1 + type0_count(header);
            break;
        case PacketType::Type1:
            packets_consumed = 1;
            break;
        case PacketType::Type2:
            execute_type2(header);
            packets_consumed = 1;
            break;
        case PacketType::Type3:
            execute_type3(header, addr + 4);
            packets_consumed = 1 + type3_count(header);
            break;
        default:
            packets_consumed = 1;
            break;
    }
    return packets_consumed;
}

void CommandProcessor::execute_type0(u32 header, GuestAddr data_addr) {
    u32 base_index = type0_base_index(header);
    u32 count = type0_count(header);
    for (u32 i = 0; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        write_register(base_index + i, value);
    }
}

void CommandProcessor::execute_type2(u32) {
    // NOP
}

void CommandProcessor::execute_type3(u32 header, GuestAddr data_addr) {
    u32 opcode = header & 0xFF;
    u32 count = type3_count(header);

    switch (static_cast<PM4Opcode>(opcode)) {
        case PM4Opcode::NOP:
            break;
        case PM4Opcode::REG_RMW:
            if (count >= 3) {
                u32 reg = read_cmd(data_addr);
                u32 and_mask = read_cmd(data_addr + 4);
                u32 or_mask = read_cmd(data_addr + 8);
                u32 value = (registers_[reg] & and_mask) | or_mask;
                write_register(reg, value);
            }
            break;
        case PM4Opcode::SET_CONSTANT:
        case PM4Opcode::SET_CONSTANT2:
        case PM4Opcode::SET_SHADER_CONSTANTS:
            handle_set_constant(data_addr, count);
            break;
        case PM4Opcode::EVENT_WRITE:
        case PM4Opcode::EVENT_WRITE_EXT:
            handle_event_write(data_addr, count);
            break;
        case PM4Opcode::MEM_WRITE:
            handle_mem_write(data_addr, count);
            break;
        case PM4Opcode::SURFACE_SYNC:
            handle_surface_sync(data_addr, count);
            break;
        default:
            break; // Ignore unknown opcodes
    }
}

// --- Direct-buffer processing (used by process_ring_buffer) ---

void CommandProcessor::process_ring_buffer(const u32* commands, size_t count) {
    if (!commands || count == 0) return;
    direct_buffer_ = commands;
    direct_buffer_size_ = count;
    direct_buffer_pos_ = 0;

    while (direct_buffer_pos_ < count) {
        u32 packets_consumed = 0;
        execute_packet_direct(commands + direct_buffer_pos_, packets_consumed);
        if (packets_consumed == 0) packets_consumed = 1;
        direct_buffer_pos_ += packets_consumed;
        packets_processed_++;
        if (frame_complete_) break;
    }

    direct_buffer_ = nullptr;
    direct_buffer_size_ = 0;
    direct_buffer_pos_ = 0;
}

u32 CommandProcessor::execute_packet_direct(const u32* packet, u32& packets_consumed) {
    u32 header = packet[0];
    PacketType type = get_packet_type(header);

    switch (type) {
        case PacketType::Type0:
            execute_type0_direct(header, packet + 1);
            packets_consumed = 1 + type0_count(header);
            break;
        case PacketType::Type1:
            packets_consumed = 1;
            break;
        case PacketType::Type2:
            execute_type2(header);
            packets_consumed = 1;
            break;
        case PacketType::Type3:
            execute_type3_direct(header, packet + 1);
            packets_consumed = 1 + type3_count(header);
            break;
        default:
            packets_consumed = 1;
            break;
    }
    return packets_consumed;
}

void CommandProcessor::execute_type0_direct(u32 header, const u32* data) {
    u32 base_index = type0_base_index(header);
    u32 count = type0_count(header);
    for (u32 i = 0; i < count; i++) {
        write_register(base_index + i, data[i]);
    }
}

void CommandProcessor::execute_type3_direct(u32 header, const u32* data) {
    u32 opcode = header & 0xFF;
    u32 count = type3_count(header);

    switch (static_cast<PM4Opcode>(opcode)) {
        case PM4Opcode::NOP:
            break;
        case PM4Opcode::REG_RMW:
            if (count >= 3) {
                u32 reg = data[0];
                u32 and_mask = data[1];
                u32 or_mask = data[2];
                u32 value = (registers_[reg] & and_mask) | or_mask;
                write_register(reg, value);
            }
            break;
        case PM4Opcode::SET_CONSTANT:
        case PM4Opcode::SET_CONSTANT2:
        case PM4Opcode::SET_SHADER_CONSTANTS:
            handle_set_constant_direct(data, count);
            break;
        case PM4Opcode::EVENT_WRITE:
        case PM4Opcode::EVENT_WRITE_EXT:
            if (count >= 1) {
                u32 event_type = data[0] & 0x3F;
                if (event_type == 0x14) {
                    frame_complete_ = true;
                    in_frame_ = false;
                    draws_this_frame_ = 0;
                }
            }
            break;
        default:
            break;
    }
}

// --- Type 3 handlers (memory-based) ---

void CommandProcessor::handle_set_constant(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    u32 info = read_cmd(data_addr);
    u32 type = (info >> 16) & 0x3;
    u32 index = info & 0x7FF;

    for (u32 i = 1; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        u32 const_index = index + i - 1;
        switch (type) {
            case 0: // ALU constants
                if (const_index < 256 * 4) {
                    memcpy(&vertex_constants_[const_index], &value, sizeof(f32));
                    memcpy(&gpu_state_.alu_constants[const_index], &value, sizeof(f32));
                    vertex_constants_dirty_ = true;
                }
                break;
            case 1: // Fetch constants
                if (const_index < 96 * 6) {
                    u32 fetch_idx = const_index / 6;
                    u32 word_idx = const_index % 6;
                    vertex_fetch_[fetch_idx].data[word_idx] = value;
                    gpu_state_.vertex_fetch_constants[const_index] = value;
                }
                break;
            case 2: // Bool constants
                if (const_index < 8) {
                    bool_constants_[const_index] = value;
                    gpu_state_.bool_constants[const_index] = value;
                    bool_constants_dirty_ = true;
                }
                break;
            case 3: // Loop constants
                if (const_index < 32) {
                    loop_constants_[const_index] = value;
                    gpu_state_.loop_constants[const_index] = value;
                    loop_constants_dirty_ = true;
                }
                break;
        }
    }
}

void CommandProcessor::handle_event_write(GuestAddr data_addr, u32 count) {
    if (count >= 1) {
        u32 event_type = read_cmd(data_addr) & 0x3F;
        if (event_type == 0x14) { // SWAP
            frame_complete_ = true;
            in_frame_ = false;
            draws_this_frame_ = 0;
        }
    }
}

void CommandProcessor::handle_mem_write(GuestAddr data_addr, u32 count) {
    if (count >= 2 && memory_) {
        GuestAddr dest = read_cmd(data_addr) & 0xFFFFFFFC;
        for (u32 i = 1; i < count; i++) {
            memory_->write_u32(dest + (i - 1) * 4, read_cmd(data_addr + i * 4));
        }
    }
}

// --- Type 3 handlers (direct-buffer) ---

void CommandProcessor::handle_set_constant_direct(const u32* data, u32 count) {
    if (count < 1) return;
    u32 info = data[0];
    u32 type = (info >> 16) & 0x3;
    u32 index = info & 0x7FF;

    for (u32 i = 1; i < count; i++) {
        u32 value = data[i];
        u32 const_index = index + i - 1;
        switch (type) {
            case 0:
                if (const_index < 256 * 4) {
                    memcpy(&vertex_constants_[const_index], &value, sizeof(f32));
                    memcpy(&gpu_state_.alu_constants[const_index], &value, sizeof(f32));
                    vertex_constants_dirty_ = true;
                }
                break;
            case 1:
                if (const_index < 96 * 6) {
                    u32 fetch_idx = const_index / 6;
                    u32 word_idx = const_index % 6;
                    vertex_fetch_[fetch_idx].data[word_idx] = value;
                    gpu_state_.vertex_fetch_constants[const_index] = value;
                }
                break;
            case 2:
                if (const_index < 8) {
                    bool_constants_[const_index] = value;
                    gpu_state_.bool_constants[const_index] = value;
                    bool_constants_dirty_ = true;
                }
                break;
            case 3:
                if (const_index < 32) {
                    loop_constants_[const_index] = value;
                    gpu_state_.loop_constants[const_index] = value;
                    loop_constants_dirty_ = true;
                }
                break;
        }
    }
}

void CommandProcessor::handle_draw_indx_direct(const u32*, u32) {}
void CommandProcessor::handle_draw_indx_auto_direct(const u32*, u32) {}

// --- Remaining memory-based handlers (stubs) ---

void CommandProcessor::handle_draw_indx(GuestAddr, u32) {}
void CommandProcessor::handle_draw_indx_2(GuestAddr, u32) {}
void CommandProcessor::handle_draw_indx_auto(GuestAddr, u32) {}
void CommandProcessor::handle_draw_indx_immd(GuestAddr, u32) {}
void CommandProcessor::handle_load_alu_constant(GuestAddr, u32) {}
void CommandProcessor::handle_load_bool_constant(GuestAddr, u32) {}
void CommandProcessor::handle_load_loop_constant(GuestAddr, u32) {}
void CommandProcessor::handle_wait_reg_mem(GuestAddr, u32) {}
void CommandProcessor::handle_indirect_buffer(GuestAddr, u32) {}
void CommandProcessor::handle_cond_write(GuestAddr, u32) {}
void CommandProcessor::handle_surface_sync(GuestAddr, u32) {}
void CommandProcessor::handle_event_write_shd(GuestAddr, u32) {}
void CommandProcessor::handle_im_load(GuestAddr, u32) {}
void CommandProcessor::handle_im_load_immediate(GuestAddr, u32) {}
void CommandProcessor::handle_draw_indx_bin(GuestAddr, u32) {}
void CommandProcessor::handle_copy_dw(GuestAddr, u32) {}
void CommandProcessor::handle_viz_query(GuestAddr, u32) {}
void CommandProcessor::handle_set_predication(GuestAddr, u32) {}
void CommandProcessor::handle_set_bin_mask(GuestAddr, u32, bool) {}
void CommandProcessor::handle_set_bin_select(GuestAddr, u32, bool) {}

// --- State update stubs ---

void CommandProcessor::update_render_state() {}
void CommandProcessor::update_shaders() {}
void CommandProcessor::update_textures() {}
void CommandProcessor::update_vertex_buffers() {}
void CommandProcessor::update_gpu_state() {}
void CommandProcessor::execute_draw(const DrawCommand&) {}
bool CommandProcessor::prepare_shaders() { return false; }
bool CommandProcessor::prepare_pipeline(const DrawCommand&) { return false; }
void CommandProcessor::set_dynamic_state() {}
void CommandProcessor::bind_vertex_buffers(const DrawCommand&) {}
void CommandProcessor::bind_index_buffer(const DrawCommand&) {}
void CommandProcessor::build_vertex_input_state(VertexInputConfig&) {}
void CommandProcessor::update_constants() {}
void CommandProcessor::bind_textures() {}

// --- State deduplication stubs ---

bool CommandProcessor::bind_pipeline_dedup(VkPipeline) { return false; }
bool CommandProcessor::bind_descriptor_set_dedup(VkDescriptorSet) { return false; }
void CommandProcessor::set_viewport_dedup(float, float, float, float, float, float) {}
void CommandProcessor::set_scissor_dedup(s32, s32, u32, u32) {}
void CommandProcessor::bind_vertex_buffers_dedup(u32, const VkBuffer*, const VkDeviceSize*) {}
void CommandProcessor::bind_index_buffer_dedup(VkBuffer, VkDeviceSize, VkIndexType) {}
void CommandProcessor::reset_bound_state() { bound_state_.reset(); }

// --- Draw batching stubs ---

void CommandProcessor::queue_draw(const DrawCommand&) {}
void CommandProcessor::flush_draw_batch() {}
bool CommandProcessor::can_merge_draw(const DrawCommand&) const { return false; }

// --- Default shader stubs ---

void CommandProcessor::create_default_shaders() {}
void CommandProcessor::use_default_shaders() {}
void CommandProcessor::cleanup_default_shaders() {}

// --- Tessellation stubs ---

bool CommandProcessor::needs_tessellation(const DrawCommand&) const { return false; }
DrawCommand CommandProcessor::tessellate_draw(const DrawCommand& cmd) { return cmd; }
void CommandProcessor::tessellate_tri_patch(std::vector<f32>&, u32) {}
void CommandProcessor::tessellate_quad_patch(std::vector<f32>&, u32) {}
DrawCommand CommandProcessor::expand_rect_list(const DrawCommand& cmd) { return cmd; }

// --- Register side effects ---

void CommandProcessor::on_register_write(u32 index, u32 value) {
    // Update gpu_state_ for registers that tests inspect
    switch (index) {
        case xenos_reg::SQ_VS_PROGRAM:
            gpu_state_.vertex_shader_addr = value;
            break;
        case xenos_reg::SQ_PS_PROGRAM:
            gpu_state_.pixel_shader_addr = value;
            break;
        default:
            break;
    }
}

void CommandProcessor::process_type0_write(u32 base_reg, const u32* data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        write_register(base_reg + i, data[i]);
    }
}

// ===========================================================================
// Default Shader SPIR-V (copied from real default_shaders.cpp)
// ===========================================================================

const std::vector<u32>& get_default_vertex_shader_spirv() {
    static const std::vector<u32> spirv = {
        0x07230203, 0x00010000, 0x00080001, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
        0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
        0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000d, 0x00030003,
        0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x00000009,
        0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000009, 0x00000000, 0x505f6c67,
        0x7469736f, 0x006e6f69, 0x00070006, 0x00000009, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
        0x00000000, 0x00070006, 0x00000009, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
        0x00070006, 0x00000009, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005,
        0x0000000b, 0x00000000, 0x00060005, 0x0000000d, 0x6f506e69, 0x69746973, 0x00006e6f, 0x00050048,
        0x00000009, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x00000009, 0x00000001, 0x0000000b,
        0x00000001, 0x00050048, 0x00000009, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x00000009,
        0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x00000009, 0x00000002, 0x00040047, 0x0000000d,
        0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
        0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008,
        0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000008, 0x00000001, 0x0004001c, 0x0000000a,
        0x00000006, 0x00000008, 0x0006001e, 0x00000009, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a,
        0x00040020, 0x0000000a, 0x00000003, 0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000003,
        0x00040015, 0x0000000c, 0x00000020, 0x00000001, 0x0004002b, 0x0000000c, 0x0000000c, 0x00000000,
        0x00040017, 0x0000000e, 0x00000006, 0x00000003, 0x00040020, 0x0000000f, 0x00000001, 0x0000000e,
        0x0004003b, 0x0000000f, 0x0000000d, 0x00000001, 0x0004002b, 0x00000006, 0x00000011, 0x3f800000,
        0x00040020, 0x00000013, 0x00000003, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
        0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000e, 0x00000010, 0x0000000d, 0x00050051,
        0x00000006, 0x00000014, 0x00000010, 0x00000000, 0x00050051, 0x00000006, 0x00000015, 0x00000010,
        0x00000001, 0x00050051, 0x00000006, 0x00000016, 0x00000010, 0x00000002, 0x00070050, 0x00000007,
        0x00000017, 0x00000014, 0x00000015, 0x00000016, 0x00000011, 0x00050041, 0x00000013, 0x00000018,
        0x0000000b, 0x0000000c, 0x0003003e, 0x00000018, 0x00000017, 0x000100fd, 0x00010038
    };
    return spirv;
}

const std::vector<u32>& get_default_pixel_shader_spirv() {
    static const std::vector<u32> spirv = {
        0x07230203, 0x00010000, 0x00080001, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
        0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
        0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
        0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
        0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f, 0x00000000, 0x00040047, 0x00000009, 0x0000001e,
        0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
        0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003,
        0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b, 0x00000006, 0x0000000a,
        0x3f800000, 0x0004002b, 0x00000006, 0x0000000b, 0x00000000, 0x0007002c, 0x00000007, 0x0000000c,
        0x0000000a, 0x0000000b, 0x0000000b, 0x0000000a, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
        0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000c, 0x000100fd, 0x00010038
    };
    return spirv;
}

} // namespace x360mu
