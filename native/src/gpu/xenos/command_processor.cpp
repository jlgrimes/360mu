/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * GPU Command Processor Implementation
 * Parses and executes PM4 command packets from the GPU ring buffer
 * 
 * PM4 is the packet format used by ATI/AMD GPUs (inherited from R500/Xenos)
 * 
 * Packet Format:
 * - Type 0: Register writes (bits 30-31 = 0)
 * - Type 2: NOP/padding (bits 30-31 = 2)
 * - Type 3: Commands (bits 30-31 = 3)
 */

#include "command_processor.h"
#include "shader_translator.h"
#include "texture.h"
#include "gpu/vulkan/vulkan_backend.h"
#include "gpu/shader_cache.h"
#include "gpu/texture_cache.h"
#include "gpu/descriptor_manager.h"
#include "memory/memory.h"
#include <cstring>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-cmdproc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[CMDPROC] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[CMDPROC ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// PM4 Packet Parsing Helpers
//=============================================================================

// Extract packet type from header (bits 30-31)
inline PacketType get_packet_type(u32 header) {
    return static_cast<PacketType>((header >> 30) & 0x3);
}

// Type 0: Register write
// Bits 0-14: Base register address
// Bit 15: One register write mode (write to same reg N times)
// Bits 16-29: Count - 1
inline u32 type0_base_index(u32 header) {
    return header & 0x7FFF;
}

inline u32 type0_count(u32 header) {
    return ((header >> 16) & 0x3FFF) + 1;
}

inline bool type0_one_reg_wr(u32 header) {
    return (header >> 15) & 1;  // Write same register multiple times
}

// Type 2: NOP (padding)
// No data - just a single dword header

// Type 3: Command packet
// Bits 0-7: Opcode
// Bit 8: Predicate flag
// Bits 16-29: Count (dwords of data following header)
inline PM4Opcode type3_opcode(u32 header) {
    return static_cast<PM4Opcode>(header & 0xFF);
}

inline u32 type3_count(u32 header) {
    return ((header >> 16) & 0x3FFF);
}

inline bool type3_predicate(u32 header) {
    return (header >> 8) & 1;
}

//=============================================================================
// Primitive Type Translation (Xenos -> Vulkan)
//=============================================================================

/**
 * Translate Xenos primitive type to Vulkan topology
 */
VkPrimitiveTopology translate_primitive_type(u32 type) {
    switch (type) {
        case 0x00: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 0x01: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;         // Point list
        case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;          // Line list
        case 0x03: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;         // Line strip
        case 0x04: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;      // Triangle list
        case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;       // Triangle fan
        case 0x06: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;     // Triangle strip
        case 0x08: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;      // Rect list (emulated)
        case 0x0D: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;      // Quad list (emulated)
        case 0x11: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        default:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/**
 * Translate Xenos primitive type enum to Vulkan topology
 */
VkPrimitiveTopology translate_primitive_type(PrimitiveType type) {
    return translate_primitive_type(static_cast<u32>(type));
}

//=============================================================================
// CommandProcessor Implementation
//=============================================================================

CommandProcessor::CommandProcessor() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
    gpu_state_ = {};
}

CommandProcessor::~CommandProcessor() {
    shutdown();
}

Status CommandProcessor::initialize(Memory* memory, VulkanBackend* vulkan,
                                    ShaderTranslator* shader_translator,
                                    TextureCacheImpl* texture_cache,
                                    ShaderCache* shader_cache,
                                    DescriptorManager* descriptor_manager) {
    memory_ = memory;
    vulkan_ = vulkan;
    shader_translator_ = shader_translator;
    texture_cache_ = texture_cache;
    shader_cache_ = shader_cache;
    descriptor_manager_ = descriptor_manager;
    
    reset();
    
    LOGI("Command processor initialized (shader_cache=%s, descriptors=%s)",
         shader_cache ? "yes" : "no", descriptor_manager ? "yes" : "no");
    return Status::Ok;
}

void CommandProcessor::shutdown() {
    memory_ = nullptr;
    vulkan_ = nullptr;
    shader_translator_ = nullptr;
    texture_cache_ = nullptr;
    shader_cache_ = nullptr;
    descriptor_manager_ = nullptr;
    current_vertex_shader_ = nullptr;
    current_pixel_shader_ = nullptr;
    current_pipeline_ = VK_NULL_HANDLE;
}

u32 CommandProcessor::read_cmd(GuestAddr addr) {
    if (memory_) {
        return memory_->read_u32(addr);
    }
    return 0;
}

void CommandProcessor::reset() {
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
    
    direct_buffer_ = nullptr;
    direct_buffer_size_ = 0;
    direct_buffer_pos_ = 0;
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

bool CommandProcessor::process(GuestAddr ring_base, u32 ring_size, u32& read_ptr, u32 write_ptr) {
    // #region agent log - Hypothesis C: Check if commands are being processed
    static int cp_log_count = 0;
    cp_log_count++;
    bool has_commands = (read_ptr != write_ptr);
    if (cp_log_count <= 20 || (has_commands && cp_log_count <= 100)) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"C\",\"location\":\"command_processor.cpp:process\",\"message\":\"CP process entry\",\"data\":{\"call\":%d,\"ring_base\":%u,\"ring_size\":%u,\"read_ptr\":%u,\"write_ptr\":%u,\"has_commands\":%d}}\n", cp_log_count, ring_base, ring_size, read_ptr, write_ptr, has_commands); fclose(f); }
    }
    // #endregion
    
    frame_complete_ = false;
    
    // #region agent log - Hypothesis C: Track packets processed
    u32 packets_in_this_call = 0;
    // #endregion
    
    // Process packets until read catches up to write
    while (read_ptr != write_ptr) {
        // Calculate address in ring buffer (ring buffer wraps)
        GuestAddr packet_addr = ring_base + (read_ptr * 4);
        
        // Execute packet and get number of dwords consumed
        u32 packets_consumed = 0;
        execute_packet(packet_addr, packets_consumed);
        
        if (packets_consumed == 0) {
            LOGE("Packet processing stalled at %08X", packet_addr);
            packets_consumed = 1;  // Skip to prevent infinite loop
        }
        
        // Advance read pointer (with wrap)
        read_ptr = (read_ptr + packets_consumed) % ring_size;
        packets_processed_++;
        packets_in_this_call++;
        
        if (frame_complete_) {
            break;
        }
    }
    
    // #region agent log - Hypothesis C: Log packets processed count
    static int cp_exit_log = 0;
    if (cp_exit_log++ < 20 || packets_in_this_call > 0) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"C\",\"location\":\"command_processor.cpp:process_exit\",\"message\":\"CP process done\",\"data\":{\"packets_processed\":%u,\"frame_complete\":%d,\"total_packets\":%llu}}\n", packets_in_this_call, frame_complete_, packets_processed_); fclose(f); }
    }
    // #endregion
    
    return frame_complete_;
}

void CommandProcessor::process_ring_buffer(const u32* commands, size_t count) {
    // Store buffer for direct access
    direct_buffer_ = commands;
    direct_buffer_size_ = count;
    direct_buffer_pos_ = 0;
    
    while (direct_buffer_pos_ < count) {
        u32 packets_consumed = 0;
        execute_packet_direct(commands + direct_buffer_pos_, packets_consumed);
        
        if (packets_consumed == 0) {
            LOGE("Packet processing stalled at position %zu", direct_buffer_pos_);
            packets_consumed = 1;  // Skip to prevent infinite loop
        }
        
        direct_buffer_pos_ += packets_consumed;
        packets_processed_++;
    }
    
    // Clear direct buffer reference
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
            // Reserved - shouldn't encounter
            LOGE("Type 1 packet encountered (reserved)");
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
            LOGE("Unknown packet type %d", static_cast<int>(type));
            packets_consumed = 1;
            break;
    }
    
    return packets_consumed;
}

void CommandProcessor::execute_type0_direct(u32 header, const u32* data) {
    u32 base_index = type0_base_index(header);
    u32 count = type0_count(header);
    bool one_reg = type0_one_reg_wr(header);
    
    process_type0_write(base_index, data, one_reg ? 1 : count);
    
    // If one_reg mode, write same value multiple times (for FIFO-style registers)
    if (one_reg) {
        for (u32 i = 0; i < count; i++) {
            write_register(base_index, data[i]);
        }
    }
}

void CommandProcessor::execute_type3_direct(u32 header, const u32* data) {
    PM4Opcode opcode = type3_opcode(header);
    u32 count = type3_count(header);
    
    // Check predication
    if (type3_predicate(header)) {
        // TODO: Check predicate register
    }
    
    switch (opcode) {
        case PM4Opcode::NOP:
            // Nothing to do
            break;
            
        case PM4Opcode::INTERRUPT:
            // Signal interrupt to CPU
            LOGD("GPU interrupt");
            break;
            
        case PM4Opcode::WAIT_FOR_IDLE:
            // Wait for GPU to finish - nothing to do in emulation
            break;
            
        case PM4Opcode::REG_RMW:
            // Read-modify-write register
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
            
        case PM4Opcode::DRAW_INDX:
            handle_draw_indx_direct(data, count);
            break;
            
        case PM4Opcode::DRAW_INDX_2:
        case PM4Opcode::DRAW_INDX_AUTO:
            handle_draw_indx_auto_direct(data, count);
            break;
            
        default:
            LOGD("Unhandled PM4 opcode: 0x%02X, count: %u", 
                 static_cast<u32>(opcode), count);
            break;
    }
}

void CommandProcessor::process_type0_write(u32 base_reg, const u32* data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        write_register(base_reg + i, data[i]);
    }
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
            // Reserved - shouldn't encounter
            LOGE("Type 1 packet encountered (reserved)");
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
            LOGE("Unknown packet type %d", static_cast<int>(type));
            packets_consumed = 1;
            break;
    }
    
    return packets_consumed;
}

void CommandProcessor::execute_type0(u32 header, GuestAddr data_addr) {
    u32 base_index = type0_base_index(header);
    u32 count = type0_count(header);
    bool one_reg = type0_one_reg_wr(header);
    
    for (u32 i = 0; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        u32 reg_index = one_reg ? base_index : (base_index + i);
        
        write_register(reg_index, value);
    }
}

void CommandProcessor::execute_type2(u32 /*header*/) {
    // NOP - nothing to do
}

void CommandProcessor::execute_type3(u32 header, GuestAddr data_addr) {
    PM4Opcode opcode = type3_opcode(header);
    u32 count = type3_count(header);
    
    // Check predication
    if (type3_predicate(header)) {
        // TODO: Check predicate register
    }
    
    switch (opcode) {
        case PM4Opcode::NOP:
            // Nothing to do
            break;
            
        case PM4Opcode::INTERRUPT:
            // Signal interrupt to CPU
            LOGD("GPU interrupt");
            break;
            
        case PM4Opcode::WAIT_FOR_IDLE:
            // Wait for GPU to finish - nothing to do in emulation
            break;
            
        case PM4Opcode::WAIT_REG_MEM:
            handle_wait_reg_mem(data_addr, count);
            break;
            
        case PM4Opcode::REG_RMW:
            // Read-modify-write register
            {
                u32 reg = read_cmd(data_addr);
                u32 and_mask = read_cmd(data_addr + 4);
                u32 or_mask = read_cmd(data_addr + 8);
                u32 value = (registers_[reg] & and_mask) | or_mask;
                write_register(reg, value);
            }
            break;
            
        case PM4Opcode::LOAD_ALU_CONSTANT:
            handle_load_alu_constant(data_addr, count);
            break;
            
        case PM4Opcode::LOAD_BOOL_CONSTANT:
            handle_load_bool_constant(data_addr, count);
            break;
            
        case PM4Opcode::LOAD_LOOP_CONSTANT:
            handle_load_loop_constant(data_addr, count);
            break;
            
        case PM4Opcode::SET_CONSTANT:
        case PM4Opcode::SET_CONSTANT2:
            handle_set_constant(data_addr, count);
            break;
            
        case PM4Opcode::SET_SHADER_CONSTANTS:
            // Similar to SET_CONSTANT but different format
            handle_set_constant(data_addr, count);
            break;
            
        case PM4Opcode::DRAW_INDX:
            handle_draw_indx(data_addr, count);
            break;
            
        case PM4Opcode::DRAW_INDX_2:
            handle_draw_indx_2(data_addr, count);
            break;
            
        case PM4Opcode::DRAW_INDX_AUTO:
            handle_draw_indx_auto(data_addr, count);
            break;
            
        case PM4Opcode::DRAW_INDX_IMMD:
            // Draw with immediate indices embedded in packet
            handle_draw_indx_immd(data_addr, count);
            break;
            
        case PM4Opcode::MEM_WRITE:
            handle_mem_write(data_addr, count);
            break;
            
        case PM4Opcode::COND_WRITE:
            handle_cond_write(data_addr, count);
            break;
            
        case PM4Opcode::EVENT_WRITE:
        case PM4Opcode::EVENT_WRITE_SHD:
        case PM4Opcode::EVENT_WRITE_EXT:
            handle_event_write(data_addr, count);
            break;
            
        case PM4Opcode::INDIRECT_BUFFER:
        case PM4Opcode::INDIRECT_BUFFER_PFD:
            handle_indirect_buffer(data_addr, count);
            break;
            
        case PM4Opcode::SURFACE_SYNC:
            handle_surface_sync(data_addr, count);
            break;
            
        case PM4Opcode::ME_INIT:
            // Microengine init - reset state
            LOGD("ME_INIT");
            break;
            
        case PM4Opcode::CP_INVALIDATE_STATE:
            // Invalidate CP state cache
            break;
            
        case PM4Opcode::VIZ_QUERY:
            // Visibility query
            break;
            
        case PM4Opcode::CONTEXT_UPDATE:
            // Context update
            break;
            
        default:
            LOGD("Unhandled PM4 opcode: 0x%02X, count: %u", 
                 static_cast<u32>(opcode), count);
            break;
    }
}

//=============================================================================
// Type 3 Packet Handlers
//=============================================================================

void CommandProcessor::handle_draw_indx(GuestAddr data_addr, u32 count) {
    if (count < 2) return;
    
    // Parse draw command
    u32 dword0 = read_cmd(data_addr);
    u32 dword1 = read_cmd(data_addr + 4);
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((dword0 >> 0) & 0x3F);
    cmd.indexed = (dword0 >> 11) & 1;
    cmd.index_count = dword1 & 0xFFFFFF;
    cmd.index_size = ((dword0 >> 6) & 1) ? 4 : 2;  // 32-bit or 16-bit indices
    
    if (cmd.indexed && count >= 3) {
        cmd.index_base = read_cmd(data_addr + 8);
    }
    
    // Update render state before drawing
    update_render_state();
    update_shaders();
    update_textures();
    update_vertex_buffers();
    
    // Execute the draw
    execute_draw(cmd);
}

void CommandProcessor::handle_draw_indx_2(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 dword0 = read_cmd(data_addr);
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((dword0 >> 0) & 0x3F);
    cmd.indexed = false;
    cmd.vertex_count = (dword0 >> 16) & 0xFFFF;
    cmd.instance_count = 1;
    
    update_render_state();
    update_shaders();
    execute_draw(cmd);
}

void CommandProcessor::handle_draw_indx_auto(GuestAddr data_addr, u32 count) {
    if (count < 2) return;
    
    // DRAW_INDEX_AUTO format:
    // data[0] = vertex count
    // data[1] = VGT_DRAW_INITIATOR (primitive type in bits 0-5)
    u32 vertex_count = read_cmd(data_addr);
    u32 draw_initiator = read_cmd(data_addr + 4);
    u32 prim_type = draw_initiator & 0x3F;
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>(prim_type);
    cmd.indexed = false;
    cmd.vertex_count = vertex_count;
    cmd.instance_count = 1;
    
    // Update state before drawing
    update_render_state();
    update_shaders();
    update_textures();
    update_vertex_buffers();
    
    // Execute the draw
    execute_draw(cmd);
    
    LOGD("DRAW_INDEX_AUTO: %u vertices, prim type %u", vertex_count, prim_type);
}

void CommandProcessor::handle_draw_indx_immd(GuestAddr data_addr, u32 count) {
    if (count < 3) return;
    
    // DRAW_INDX_IMMD format:
    // data[0] = VGT_DRAW_INITIATOR (primitive type, source select)
    // data[1] = index count << 16 | index type (0=u16, 1=u32)
    // data[2...] = immediate index data
    
    u32 dword0 = read_cmd(data_addr);
    u32 dword1 = read_cmd(data_addr + 4);
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((dword0 >> 0) & 0x3F);
    cmd.indexed = true;
    cmd.index_count = dword1 >> 16;
    cmd.index_size = ((dword0 >> 6) & 1) ? 4 : 2;
    cmd.instance_count = 1;
    
    // The indices follow immediately in the packet
    // For now, we don't have direct access to them in Vulkan
    // This would require copying them to a staging buffer
    
    update_render_state();
    update_shaders();
    execute_draw(cmd);
    
    LOGD("DRAW_INDX_IMMD: %u indices", cmd.index_count);
}

//=============================================================================
// Direct Buffer Handlers (for testing)
//=============================================================================

void CommandProcessor::handle_draw_indx_direct(const u32* data, u32 count) {
    if (count < 2) return;
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((data[0] >> 0) & 0x3F);
    cmd.indexed = (data[0] >> 11) & 1;
    cmd.index_count = data[1] & 0xFFFFFF;
    cmd.index_size = ((data[0] >> 6) & 1) ? 4 : 2;
    cmd.instance_count = 1;
    
    if (cmd.indexed && count >= 3) {
        cmd.index_base = data[2];
    }
    
    update_render_state();
    update_shaders();
    execute_draw(cmd);
}

void CommandProcessor::handle_draw_indx_auto_direct(const u32* data, u32 count) {
    if (count < 2) return;
    
    // data[0] = vertex count
    // data[1] = VGT_DRAW_INITIATOR (primitive type in bits 0-5)
    u32 vertex_count = data[0];
    u32 prim_type = data[1] & 0x3F;
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>(prim_type);
    cmd.indexed = false;
    cmd.vertex_count = vertex_count;
    cmd.instance_count = 1;
    
    update_render_state();
    update_shaders();
    execute_draw(cmd);
}

void CommandProcessor::handle_set_constant_direct(const u32* data, u32 count) {
    if (count < 1) return;
    
    u32 info = data[0];
    u32 type = (info >> 16) & 0x3;  // 0=ALU, 1=Fetch, 2=Bool, 3=Loop
    u32 index = info & 0x1FF;
    
    // Read constant values
    for (u32 i = 1; i < count; i++) {
        u32 value = data[i];
        u32 const_index = index + i - 1;
        
        switch (type) {
            case 0:  // ALU (float) constants
                if (const_index < 256 * 4) {
                    memcpy(&vertex_constants_[const_index], &value, sizeof(f32));
                    // Also update gpu_state_
                    memcpy(&gpu_state_.alu_constants[const_index], &value, sizeof(f32));
                }
                break;
                
            case 1:  // Fetch constants
                if (const_index < 96 * 6) {
                    u32 fetch_idx = const_index / 6;
                    u32 word_idx = const_index % 6;
                    vertex_fetch_[fetch_idx].data[word_idx] = value;
                    gpu_state_.vertex_fetch_constants[const_index] = value;
                }
                break;
                
            case 2:  // Bool constants
                if (const_index < bool_constants_.size()) {
                    bool_constants_[const_index] = value;
                    if (const_index < 8) {
                        gpu_state_.bool_constants[const_index] = value;
                    }
                }
                break;
                
            case 3:  // Loop constants
                if (const_index < loop_constants_.size()) {
                    loop_constants_[const_index] = value;
                    gpu_state_.loop_constants[const_index] = value;
                }
                break;
        }
    }
}

void CommandProcessor::handle_load_alu_constant(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 info = read_cmd(data_addr);
    u32 start_offset = info & 0x1FF;         // Starting constant index
    u32 size_vec4 = ((info >> 16) & 0x1FF);  // Number of vec4s
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    // Determine if this is vertex or pixel constants
    bool is_pixel = (info >> 31) & 1;
    f32* dest = is_pixel ? pixel_constants_.data() : vertex_constants_.data();
    
    // Load constants from memory
    for (u32 i = 0; i < size_vec4 && (start_offset + i) < 256; i++) {
        u32 offset = (start_offset + i) * 4;
        for (u32 j = 0; j < 4; j++) {
            u32 raw = memory_->read_u32(src_addr + (i * 16) + (j * 4));
            memcpy(&dest[offset + j], &raw, sizeof(f32));
        }
    }
}

void CommandProcessor::handle_load_bool_constant(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 info = read_cmd(data_addr);
    u32 start_bit = info & 0xFF;
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    // Load boolean constants (packed as bits)
    u32 words_to_load = (count - 1);
    for (u32 i = 0; i < words_to_load && (start_bit / 32 + i) < bool_constants_.size(); i++) {
        bool_constants_[start_bit / 32 + i] = memory_->read_u32(src_addr + i * 4);
    }
}

void CommandProcessor::handle_load_loop_constant(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 info = read_cmd(data_addr);
    u32 start_index = info & 0x1F;
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    // Load loop constants
    for (u32 i = 0; i < (count - 1) && (start_index + i) < loop_constants_.size(); i++) {
        loop_constants_[start_index + i] = memory_->read_u32(src_addr + i * 4);
    }
}

void CommandProcessor::handle_set_constant(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 info = read_cmd(data_addr);
    u32 type = (info >> 16) & 0x3;  // 0=ALU, 1=Fetch, 2=Bool, 3=Loop
    u32 index = info & 0x1FF;
    
    // Read constant values
    for (u32 i = 1; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        u32 const_index = index + i - 1;
        
        switch (type) {
            case 0:  // ALU (float) constants
                if (const_index < 256 * 4) {
                    memcpy(&vertex_constants_[const_index], &value, sizeof(f32));
                }
                break;
                
            case 1:  // Fetch constants
                // Store in appropriate fetch constant array
                if (const_index < 96 * 6) {
                    u32 fetch_idx = const_index / 6;
                    u32 word_idx = const_index % 6;
                    vertex_fetch_[fetch_idx].data[word_idx] = value;
                }
                break;
                
            case 2:  // Bool constants
                if (const_index < bool_constants_.size()) {
                    bool_constants_[const_index] = value;
                }
                break;
                
            case 3:  // Loop constants
                if (const_index < loop_constants_.size()) {
                    loop_constants_[const_index] = value;
                }
                break;
        }
    }
}

void CommandProcessor::handle_event_write(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    u32 event_info = read_cmd(data_addr);
    u32 event_type = event_info & 0x3F;
    
    // Event types we care about
    constexpr u32 EVENT_SWAP = 0x14;      // Swap buffers
    constexpr u32 EVENT_CACHE_FLUSH = 0x16;
    constexpr u32 EVENT_VS_DONE = 0x28;
    constexpr u32 EVENT_PS_DONE = 0x29;
    
    switch (event_type) {
        case EVENT_SWAP:
            // Frame complete
            frame_complete_ = true;
            in_frame_ = false;
            LOGD("Frame complete: %llu draws", draws_this_frame_);
            draws_this_frame_ = 0;
            break;
            
        case EVENT_CACHE_FLUSH:
            // Flush caches
            break;
            
        case EVENT_VS_DONE:
        case EVENT_PS_DONE:
            // Shader done events
            break;
            
        default:
            break;
    }
}

void CommandProcessor::handle_mem_write(GuestAddr data_addr, u32 count) {
    if (count < 2) return;
    
    GuestAddr dest_addr = read_cmd(data_addr);
    u32 value = read_cmd(data_addr + 4);
    
    // Write value to memory
    memory_->write_u32(dest_addr, value);
}

void CommandProcessor::handle_wait_reg_mem(GuestAddr data_addr, u32 count) {
    if (count < 5) return;
    
    u32 wait_info = read_cmd(data_addr);
    u32 poll_addr_lo = read_cmd(data_addr + 4);
    u32 poll_addr_hi = read_cmd(data_addr + 8);
    u32 reference = read_cmd(data_addr + 12);
    u32 mask = read_cmd(data_addr + 16);
    
    bool mem_space = (wait_info >> 4) & 1;  // 0=register, 1=memory
    u32 function = wait_info & 0x7;
    
    // In emulation, we assume the condition is already met
    // Real hardware would spin until condition is satisfied
    (void)poll_addr_lo;
    (void)poll_addr_hi;
    (void)reference;
    (void)mask;
    (void)mem_space;
    (void)function;
}

void CommandProcessor::handle_indirect_buffer(GuestAddr data_addr, u32 count) {
    if (count < 2) return;
    
    GuestAddr ib_addr = read_cmd(data_addr);
    u32 ib_size = read_cmd(data_addr + 4) & 0xFFFFF;  // Size in dwords
    
    // Execute commands from indirect buffer
    u32 ib_read = 0;
    while (ib_read < ib_size) {
        u32 consumed = 0;
        execute_packet(ib_addr + ib_read * 4, consumed);
        ib_read += consumed;
        
        if (consumed == 0) break;  // Prevent infinite loop
    }
}

void CommandProcessor::handle_cond_write(GuestAddr data_addr, u32 count) {
    if (count < 4) return;
    
    // COND_WRITE format:
    // data[0] = function (compare operation)
    // data[1] = poll address (register or memory)
    // data[2] = reference value
    // data[3] = write address
    // data[4] = write value
    
    u32 func_info = read_cmd(data_addr);
    u32 poll_addr = read_cmd(data_addr + 4);
    u32 reference = read_cmd(data_addr + 8);
    u32 write_addr = read_cmd(data_addr + 12);
    u32 write_value = (count >= 5) ? read_cmd(data_addr + 16) : 0;
    
    bool mem_space = (func_info >> 4) & 1;  // 0=register, 1=memory
    u32 function = func_info & 0x7;
    
    // Read current value
    u32 current_value;
    if (mem_space && memory_) {
        current_value = memory_->read_u32(poll_addr);
    } else {
        current_value = get_register(poll_addr);
    }
    
    // Evaluate condition
    bool condition_met = false;
    switch (function) {
        case 0: condition_met = true; break;                           // Always
        case 1: condition_met = (current_value < reference); break;    // Less
        case 2: condition_met = (current_value <= reference); break;   // LessEqual
        case 3: condition_met = (current_value == reference); break;   // Equal
        case 4: condition_met = (current_value != reference); break;   // NotEqual
        case 5: condition_met = (current_value >= reference); break;   // GreaterEqual
        case 6: condition_met = (current_value > reference); break;    // Greater
        default: break;
    }
    
    // Perform conditional write
    if (condition_met) {
        bool write_mem = (func_info >> 8) & 1;
        if (write_mem && memory_) {
            memory_->write_u32(write_addr, write_value);
        } else {
            write_register(write_addr, write_value);
        }
    }
}

void CommandProcessor::handle_surface_sync(GuestAddr data_addr, u32 count) {
    if (count < 1) return;
    
    // SURFACE_SYNC ensures all pending surface operations complete
    // This is used for synchronization between render passes
    
    u32 sync_info = read_cmd(data_addr);
    (void)sync_info;  // Currently a no-op in emulation
    
    // In a real implementation, this would:
    // - Flush render target caches
    // - Wait for outstanding draws to complete
    // - Invalidate texture caches if needed
    
    LOGD("Surface sync");
}

//=============================================================================
// State Management
//=============================================================================

void CommandProcessor::update_render_state() {
    // Update viewport from registers
    u32 pa_cl_vte_cntl = registers_[xenos_reg::PA_CL_VTE_CNTL];
    
    if (pa_cl_vte_cntl & 1) {  // VPORT_X_SCALE_ENA
        u32 x_scale_raw = registers_[xenos_reg::PA_CL_VPORT_XSCALE];
        u32 x_offset_raw = registers_[xenos_reg::PA_CL_VPORT_XOFFSET];
        u32 y_scale_raw = registers_[xenos_reg::PA_CL_VPORT_YSCALE];
        u32 y_offset_raw = registers_[xenos_reg::PA_CL_VPORT_YOFFSET];
        u32 z_scale_raw = registers_[xenos_reg::PA_CL_VPORT_ZSCALE];
        u32 z_offset_raw = registers_[xenos_reg::PA_CL_VPORT_ZOFFSET];
        
        memcpy(&render_state_.viewport_width, &x_scale_raw, sizeof(f32));
        memcpy(&render_state_.viewport_x, &x_offset_raw, sizeof(f32));
        memcpy(&render_state_.viewport_height, &y_scale_raw, sizeof(f32));
        memcpy(&render_state_.viewport_y, &y_offset_raw, sizeof(f32));
        memcpy(&render_state_.viewport_z_max, &z_scale_raw, sizeof(f32));
        memcpy(&render_state_.viewport_z_min, &z_offset_raw, sizeof(f32));
    }
    
    // Update scissor
    u32 scissor_tl = registers_[xenos_reg::PA_SC_WINDOW_SCISSOR_TL];
    u32 scissor_br = registers_[xenos_reg::PA_SC_WINDOW_SCISSOR_BR];
    
    render_state_.scissor_left = scissor_tl & 0x7FFF;
    render_state_.scissor_top = (scissor_tl >> 16) & 0x7FFF;
    render_state_.scissor_right = scissor_br & 0x7FFF;
    render_state_.scissor_bottom = (scissor_br >> 16) & 0x7FFF;
    
    // Update depth state
    u32 rb_depthcontrol = registers_[xenos_reg::RB_DEPTHCONTROL];
    render_state_.depth_test = (rb_depthcontrol >> 1) & 1;
    render_state_.depth_write = (rb_depthcontrol >> 2) & 1;
    render_state_.depth_func = (rb_depthcontrol >> 4) & 0x7;
    
    // Update blend state
    u32 rb_blendcontrol = registers_[xenos_reg::RB_BLENDCONTROL];
    render_state_.blend_enable = (rb_blendcontrol >> 0) & 1;
    render_state_.blend_src = (rb_blendcontrol >> 0) & 0x1F;
    render_state_.blend_dst = (rb_blendcontrol >> 8) & 0x1F;
    render_state_.blend_op = (rb_blendcontrol >> 5) & 0x7;
    
    // Update cull mode
    u32 pa_su_sc_mode_cntl = registers_[xenos_reg::PA_SU_SC_MODE_CNTL];
    render_state_.cull_mode = (pa_su_sc_mode_cntl >> 0) & 0x3;
    render_state_.front_ccw = (pa_su_sc_mode_cntl >> 2) & 1;
    
    // Update render target info
    u32 rb_color_info = registers_[xenos_reg::RB_COLOR_INFO];
    render_state_.color_target_address = (rb_color_info & 0xFFFFF) << 12;
    render_state_.color_format = static_cast<SurfaceFormat>((rb_color_info >> 20) & 0xF);
    
    u32 rb_surface_info = registers_[xenos_reg::RB_SURFACE_INFO];
    render_state_.color_pitch = rb_surface_info & 0x3FFF;
}

void CommandProcessor::update_shaders() {
    // Get shader addresses from registers
    u32 sq_vs_program = registers_[xenos_reg::SQ_VS_PROGRAM];
    u32 sq_ps_program = registers_[xenos_reg::SQ_PS_PROGRAM];
    
    render_state_.vertex_shader_address = (sq_vs_program & 0xFFFFF) << 8;
    render_state_.pixel_shader_address = (sq_ps_program & 0xFFFFF) << 8;
}

void CommandProcessor::update_textures() {
    // Copy texture fetch constants
    for (u32 i = 0; i < 32; i++) {
        render_state_.texture_fetch[i] = texture_fetch_[i];
    }
}

void CommandProcessor::update_vertex_buffers() {
    // Copy vertex fetch constants
    for (u32 i = 0; i < 96; i++) {
        render_state_.vertex_fetch[i] = vertex_fetch_[i];
    }
}

void CommandProcessor::update_gpu_state() {
    // Update GpuState from registers
    
    // Shader addresses
    gpu_state_.vertex_shader_addr = registers_[xenos_reg::SQ_VS_PROGRAM];
    gpu_state_.pixel_shader_addr = registers_[xenos_reg::SQ_PS_PROGRAM];
    
    // Render target info
    gpu_state_.rb_color_info[0] = registers_[xenos_reg::RB_COLOR_INFO];
    gpu_state_.rb_color_info[1] = registers_[xenos_reg::RB_COLOR1_INFO];
    gpu_state_.rb_color_info[2] = registers_[xenos_reg::RB_COLOR2_INFO];
    gpu_state_.rb_color_info[3] = registers_[xenos_reg::RB_COLOR3_INFO];
    gpu_state_.rb_depth_info = registers_[xenos_reg::RB_DEPTH_INFO];
    gpu_state_.rb_surface_info = registers_[xenos_reg::RB_SURFACE_INFO];
    
    // Viewport transform
    u32 x_scale_raw = registers_[xenos_reg::PA_CL_VPORT_XSCALE];
    u32 x_offset_raw = registers_[xenos_reg::PA_CL_VPORT_XOFFSET];
    u32 y_scale_raw = registers_[xenos_reg::PA_CL_VPORT_YSCALE];
    u32 y_offset_raw = registers_[xenos_reg::PA_CL_VPORT_YOFFSET];
    u32 z_scale_raw = registers_[xenos_reg::PA_CL_VPORT_ZSCALE];
    u32 z_offset_raw = registers_[xenos_reg::PA_CL_VPORT_ZOFFSET];
    
    memcpy(&gpu_state_.viewport_scale[0], &x_scale_raw, sizeof(f32));
    memcpy(&gpu_state_.viewport_scale[1], &y_scale_raw, sizeof(f32));
    memcpy(&gpu_state_.viewport_scale[2], &z_scale_raw, sizeof(f32));
    gpu_state_.viewport_scale[3] = 1.0f;
    
    memcpy(&gpu_state_.viewport_offset[0], &x_offset_raw, sizeof(f32));
    memcpy(&gpu_state_.viewport_offset[1], &y_offset_raw, sizeof(f32));
    memcpy(&gpu_state_.viewport_offset[2], &z_offset_raw, sizeof(f32));
    gpu_state_.viewport_offset[3] = 0.0f;
    
    // Rasterizer state
    gpu_state_.pa_su_sc_mode_cntl = registers_[xenos_reg::PA_SU_SC_MODE_CNTL];
    gpu_state_.pa_cl_clip_cntl = registers_[xenos_reg::PA_CL_CLIP_CNTL];
    
    // Copy ALU constants
    memcpy(gpu_state_.alu_constants, vertex_constants_.data(), sizeof(gpu_state_.alu_constants));
    
    // Copy bool/loop constants
    for (u32 i = 0; i < 8; i++) {
        gpu_state_.bool_constants[i] = bool_constants_[i];
    }
    for (u32 i = 0; i < 32; i++) {
        gpu_state_.loop_constants[i] = loop_constants_[i];
    }
    
    // Copy fetch constants
    for (u32 i = 0; i < 96; i++) {
        for (u32 j = 0; j < 6; j++) {
            gpu_state_.vertex_fetch_constants[i * 6 + j] = vertex_fetch_[i].data[j];
        }
    }
    for (u32 i = 0; i < 32; i++) {
        for (u32 j = 0; j < 6; j++) {
            gpu_state_.texture_fetch_constants[i * 6 + j] = texture_fetch_[i].data[j];
        }
    }
}

void CommandProcessor::execute_draw(const DrawCommand& cmd) {
    if (!vulkan_) return;
    
    // Start frame if not already in one
    if (!in_frame_) {
        if (vulkan_->begin_frame() != Status::Ok) {
            LOGE("Failed to begin frame");
            return;
        }
        in_frame_ = true;
        current_frame_index_ = (current_frame_index_ + 1) % 3;
    }
    
    // Prepare shaders from current GPU state
    if (!prepare_shaders()) {
        LOGD("Draw skipped: shader preparation failed");
        return;
    }
    
    // Prepare pipeline for the draw
    if (!prepare_pipeline(cmd)) {
        LOGD("Draw skipped: pipeline preparation failed");
        return;
    }
    
    // Update shader constants
    update_constants();
    
    // Bind textures
    bind_textures();
    
    // Bind descriptor set
    if (descriptor_manager_) {
        VkDescriptorSet desc_set = descriptor_manager_->begin_frame(current_frame_index_);
        if (desc_set != VK_NULL_HANDLE) {
            vulkan_->bind_descriptor_set(desc_set);
        }
    }
    
    // Bind vertex and index buffers
    bind_vertex_buffers(cmd);
    if (cmd.indexed) {
        bind_index_buffer(cmd);
    }
    
    // Execute draw
    if (cmd.indexed) {
        vulkan_->draw_indexed(cmd.index_count, cmd.instance_count, cmd.start_index, 
                             cmd.base_vertex, 0);
    } else {
        u32 vertex_count = cmd.vertex_count > 0 ? cmd.vertex_count : cmd.index_count;
        vulkan_->draw(vertex_count, cmd.instance_count, 0, 0);
    }
    
    draws_this_frame_++;
    
    LOGD("Draw: %s, %u %s (pipeline=%p)", 
         cmd.indexed ? "indexed" : "non-indexed",
         cmd.indexed ? cmd.index_count : cmd.vertex_count,
         cmd.indexed ? "indices" : "vertices",
         (void*)current_pipeline_);
}

bool CommandProcessor::prepare_shaders() {
    if (!shader_cache_ || !memory_) {
        // No shader cache - use fallback behavior
        current_vertex_shader_ = nullptr;
        current_pixel_shader_ = nullptr;
        return true;  // Allow draw to proceed without shaders for testing
    }
    
    // Get shader addresses from GPU state
    GuestAddr vs_addr = render_state_.vertex_shader_address;
    GuestAddr ps_addr = render_state_.pixel_shader_address;
    
    if (vs_addr == 0 || ps_addr == 0) {
        LOGD("No shader addresses set (vs=%08x, ps=%08x)", vs_addr, ps_addr);
        return false;
    }
    
    // Get shader microcode from guest memory
    const void* vs_microcode = memory_->get_host_ptr(vs_addr);
    const void* ps_microcode = memory_->get_host_ptr(ps_addr);
    
    if (!vs_microcode || !ps_microcode) {
        LOGE("Failed to translate shader addresses");
        return false;
    }
    
    // Get shader size from registers (or use default)
    // The size is typically determined by control flow instructions
    u32 vs_size = 2048;  // Default size, would be parsed from headers
    u32 ps_size = 2048;
    
    // Get or compile shaders
    current_vertex_shader_ = shader_cache_->get_shader(vs_microcode, vs_size, ShaderType::Vertex);
    current_pixel_shader_ = shader_cache_->get_shader(ps_microcode, ps_size, ShaderType::Pixel);
    
    if (!current_vertex_shader_ || !current_pixel_shader_) {
        LOGD("Failed to get shaders");
        return false;
    }
    
    return true;
}

bool CommandProcessor::prepare_pipeline(const DrawCommand& cmd) {
    // Build pipeline key from current state
    PipelineKey key{};
    
    if (current_vertex_shader_) {
        key.vertex_shader_hash = current_vertex_shader_->hash;
    }
    if (current_pixel_shader_) {
        key.pixel_shader_hash = current_pixel_shader_->hash;
    }
    
    // Map primitive type
    switch (cmd.primitive_type) {
        case PrimitiveType::PointList:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case PrimitiveType::LineList:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case PrimitiveType::LineStrip:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case PrimitiveType::TriangleList:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        case PrimitiveType::TriangleFan:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            break;
        case PrimitiveType::TriangleStrip:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        default:
            key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
    }
    
    // Depth state
    key.depth_test_enable = render_state_.depth_test ? VK_TRUE : VK_FALSE;
    key.depth_write_enable = render_state_.depth_write ? VK_TRUE : VK_FALSE;
    
    // Map depth function
    static const VkCompareOp depth_funcs[] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS,
    };
    key.depth_compare_op = depth_funcs[render_state_.depth_func & 0x7];
    
    // Blend state
    key.blend_enable = render_state_.blend_enable ? VK_TRUE : VK_FALSE;
    key.src_color_blend = VK_BLEND_FACTOR_SRC_ALPHA;
    key.dst_color_blend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    key.color_blend_op = VK_BLEND_OP_ADD;
    
    // Cull mode
    switch (render_state_.cull_mode) {
        case 0: key.cull_mode = VK_CULL_MODE_NONE; break;
        case 1: key.cull_mode = VK_CULL_MODE_FRONT_BIT; break;
        case 2: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
        default: key.cull_mode = VK_CULL_MODE_NONE; break;
    }
    
    key.front_face = render_state_.front_ccw ? 
                     VK_FRONT_FACE_COUNTER_CLOCKWISE : 
                     VK_FRONT_FACE_CLOCKWISE;
    
    // Get or create pipeline
    if (shader_cache_ && current_vertex_shader_ && current_pixel_shader_) {
        current_pipeline_ = shader_cache_->get_pipeline(
            current_vertex_shader_, current_pixel_shader_, key);
        
        if (current_pipeline_ != VK_NULL_HANDLE) {
            vulkan_->bind_pipeline(current_pipeline_);
            return true;
        }
    }
    
    // Fallback: use default pipeline from Vulkan backend if available
    return true;  // Allow draw even without pipeline for testing
}

void CommandProcessor::bind_vertex_buffers(const DrawCommand& cmd) {
    // For now, vertex data comes through fetch constants
    // A full implementation would:
    // 1. Parse vertex fetch constants to get buffer addresses and formats
    // 2. Create/upload vertex buffers to GPU
    // 3. Bind them with vkCmdBindVertexBuffers
    
    // The vertex shader uses vfetch instructions that read from fetch constants
    // This is handled by the shader constants uniform buffer
}

void CommandProcessor::bind_index_buffer(const DrawCommand& cmd) {
    if (!cmd.indexed || cmd.index_base == 0 || !memory_) {
        return;
    }
    
    // Get index data from guest memory
    const void* index_data = memory_->get_host_ptr(cmd.index_base);
    if (!index_data) {
        LOGE("Failed to get host pointer for index buffer address %08x", cmd.index_base);
        return;
    }
    
    // Calculate index buffer size
    u64 index_size = cmd.index_count * cmd.index_size;
    
    // Create and upload index buffer
    // For now, this is a placeholder - a full implementation would
    // cache index buffers and upload them efficiently
    VulkanBuffer ib = vulkan_->create_buffer(
        index_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    if (ib.buffer != VK_NULL_HANDLE && ib.mapped) {
        memcpy(ib.mapped, index_data, index_size);
        
        VkIndexType index_type = (cmd.index_size == 4) ? 
                                  VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vulkan_->bind_index_buffer(ib.buffer, 0, index_type);
        
        // Note: In production, we'd cache this buffer and destroy it later
        // For now, we're leaking it - a proper implementation needs buffer pooling
    }
}

void CommandProcessor::update_constants() {
    if (!descriptor_manager_) return;
    
    // Update vertex shader constants
    descriptor_manager_->update_vertex_constants(
        current_frame_index_, 
        vertex_constants_.data(), 
        static_cast<u32>(vertex_constants_.size())
    );
    
    // Update pixel shader constants
    descriptor_manager_->update_pixel_constants(
        current_frame_index_,
        pixel_constants_.data(),
        static_cast<u32>(pixel_constants_.size())
    );
    
    // Update bool constants
    descriptor_manager_->update_bool_constants(
        current_frame_index_,
        bool_constants_.data(),
        static_cast<u32>(bool_constants_.size())
    );
    
    // Update loop constants
    descriptor_manager_->update_loop_constants(
        current_frame_index_,
        loop_constants_.data(),
        static_cast<u32>(loop_constants_.size())
    );
}

void CommandProcessor::bind_textures() {
    if (!texture_cache_ || !descriptor_manager_) return;
    
    std::array<VkImageView, 16> views{};
    std::array<VkSampler, 16> samplers{};
    u32 texture_count = 0;
    
    // Bind textures from fetch constants
    for (u32 i = 0; i < 16; i++) {
        const FetchConstant& fetch = texture_fetch_[i];
        
        // Check if this texture slot is used
        if (fetch.texture_address() == 0) {
            continue;
        }
        
        // Get or create texture
        const CachedTexture* tex = texture_cache_->get_texture(fetch);
        if (tex && tex->is_valid()) {
            views[i] = tex->view;
            
            // Get sampler (use default for now)
            VkSamplerConfig sampler_config{};
            sampler_config.min_filter = VK_FILTER_LINEAR;
            sampler_config.mag_filter = VK_FILTER_LINEAR;
            sampler_config.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampler_config.address_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_config.address_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_config.address_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_config.max_anisotropy = 1.0f;
            
            samplers[i] = texture_cache_->get_sampler(sampler_config);
            texture_count = i + 1;
        }
    }
    
    if (texture_count > 0) {
        descriptor_manager_->bind_textures(
            current_frame_index_,
            views.data(),
            samplers.data(),
            texture_count
        );
    }
}

void CommandProcessor::on_register_write(u32 index, u32 value) {
    // Handle side effects of register writes
    
    switch (index) {
        case xenos_reg::CP_RB_WPTR:
            // Ring buffer write pointer updated - may need to process commands
            break;
            
        case xenos_reg::SQ_VS_PROGRAM:
            // Vertex shader address updated
            gpu_state_.vertex_shader_addr = value;
            break;
            
        case xenos_reg::SQ_PS_PROGRAM:
            // Pixel shader address updated
            gpu_state_.pixel_shader_addr = value;
            break;
            
        case xenos_reg::RB_COLOR_INFO:
            gpu_state_.rb_color_info[0] = value;
            break;
            
        case xenos_reg::RB_COLOR1_INFO:
            gpu_state_.rb_color_info[1] = value;
            break;
            
        case xenos_reg::RB_COLOR2_INFO:
            gpu_state_.rb_color_info[2] = value;
            break;
            
        case xenos_reg::RB_COLOR3_INFO:
            gpu_state_.rb_color_info[3] = value;
            break;
            
        case xenos_reg::RB_DEPTH_INFO:
            gpu_state_.rb_depth_info = value;
            break;
            
        case xenos_reg::RB_SURFACE_INFO:
            gpu_state_.rb_surface_info = value;
            break;
            
        case xenos_reg::PA_SU_SC_MODE_CNTL:
            gpu_state_.pa_su_sc_mode_cntl = value;
            break;
            
        case xenos_reg::PA_CL_CLIP_CNTL:
            gpu_state_.pa_cl_clip_cntl = value;
            break;
            
        case xenos_reg::VGT_DRAW_INITIATOR:
            // Draw initiated via register write
            {
                DrawCommand cmd = {};
                cmd.primitive_type = static_cast<PrimitiveType>(value & 0x3F);
                cmd.indexed = (value >> 11) & 1;
                cmd.vertex_count = registers_[xenos_reg::VGT_IMMED_DATA];
                cmd.instance_count = 1;
                
                update_render_state();
                execute_draw(cmd);
            }
            break;
            
        case xenos_reg::RB_COPY_CONTROL:
            // Resolve initiated
            if (value & 1) {
                // TODO: Implement resolve
                LOGD("Resolve triggered");
            }
            break;
            
        default:
            break;
    }
}

} // namespace x360mu
