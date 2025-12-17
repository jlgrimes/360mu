/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * GPU Command Processor Implementation
 * Parses and executes PM4 command packets from the GPU ring buffer
 * 
 * PM4 is the packet format used by ATI/AMD GPUs (inherited from R500)
 */

#include "command_processor.h"
#include "shader_translator.h"
#include "texture.h"
#include "gpu/vulkan/vulkan_backend.h"
#include "memory/memory.h"
#include <cstring>

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
// PM4 Packet Types
//=============================================================================

// Extract packet type from header (bits 30-31)
inline PacketType get_packet_type(u32 header) {
    return static_cast<PacketType>((header >> 30) & 0x3);
}

// Type 0: Register write
// Bits 0-15: Base register address
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
// No data

// Type 3: Command packet
// Bits 0-7: Opcode
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
// CommandProcessor Implementation
//=============================================================================

CommandProcessor::CommandProcessor() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
}

CommandProcessor::~CommandProcessor() {
    shutdown();
}

Status CommandProcessor::initialize(Memory* memory, VulkanBackend* vulkan,
                                    ShaderTranslator* shader_translator,
                                    TextureCache* texture_cache) {
    memory_ = memory;
    vulkan_ = vulkan;
    shader_translator_ = shader_translator;
    texture_cache_ = texture_cache;
    
    reset();
    
    LOGI("Command processor initialized");
    return Status::Ok;
}

void CommandProcessor::shutdown() {
    memory_ = nullptr;
    vulkan_ = nullptr;
    shader_translator_ = nullptr;
    texture_cache_ = nullptr;
}

void CommandProcessor::reset() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
    
    render_state_ = {};
    frame_complete_ = false;
    in_frame_ = false;
    packets_processed_ = 0;
    draws_this_frame_ = 0;
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
    frame_complete_ = false;
    
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
        
        if (frame_complete_) {
            break;
        }
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
            
        case PM4Opcode::DRAW_INDX_IMMD:
            // Draw with immediate indices
            handle_draw_indx(data_addr, count);
            break;
            
        case PM4Opcode::MEM_WRITE:
            handle_mem_write(data_addr, count);
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
    
    update_render_state();
    update_shaders();
    execute_draw(cmd);
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

void CommandProcessor::execute_draw(const DrawCommand& cmd) {
    if (!vulkan_) return;
    
    // Start frame if not already in one
    if (!in_frame_) {
        if (vulkan_->begin_frame() != Status::Ok) {
            LOGE("Failed to begin frame");
            return;
        }
        in_frame_ = true;
    }
    
    // Get or translate shaders
    // TODO: Implement shader binding
    
    // Configure pipeline state based on render_state_
    PipelineState pipeline_state = {};
    
    // Map primitive type
    switch (cmd.primitive_type) {
        case PrimitiveType::PointList:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case PrimitiveType::LineList:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case PrimitiveType::LineStrip:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case PrimitiveType::TriangleList:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        case PrimitiveType::TriangleFan:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            break;
        case PrimitiveType::TriangleStrip:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        default:
            pipeline_state.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
    }
    
    // Depth state
    pipeline_state.depth_test_enable = render_state_.depth_test ? VK_TRUE : VK_FALSE;
    pipeline_state.depth_write_enable = render_state_.depth_write ? VK_TRUE : VK_FALSE;
    
    // Map depth function
    static const VkCompareOp depth_funcs[] = {
        VK_COMPARE_OP_NEVER,          // 0
        VK_COMPARE_OP_LESS,           // 1
        VK_COMPARE_OP_EQUAL,          // 2
        VK_COMPARE_OP_LESS_OR_EQUAL,  // 3
        VK_COMPARE_OP_GREATER,        // 4
        VK_COMPARE_OP_NOT_EQUAL,      // 5
        VK_COMPARE_OP_GREATER_OR_EQUAL, // 6
        VK_COMPARE_OP_ALWAYS,         // 7
    };
    pipeline_state.depth_compare_op = depth_funcs[render_state_.depth_func & 0x7];
    
    // Blend state
    pipeline_state.blend_enable = render_state_.blend_enable ? VK_TRUE : VK_FALSE;
    
    // Cull mode
    switch (render_state_.cull_mode) {
        case 0:  // None
            pipeline_state.cull_mode = VK_CULL_MODE_NONE;
            break;
        case 1:  // Front
            pipeline_state.cull_mode = VK_CULL_MODE_FRONT_BIT;
            break;
        case 2:  // Back
            pipeline_state.cull_mode = VK_CULL_MODE_BACK_BIT;
            break;
        default:
            pipeline_state.cull_mode = VK_CULL_MODE_NONE;
            break;
    }
    
    pipeline_state.front_face = render_state_.front_ccw ? 
                                VK_FRONT_FACE_COUNTER_CLOCKWISE : 
                                VK_FRONT_FACE_CLOCKWISE;
    
    // Execute draw
    if (cmd.indexed) {
        vulkan_->draw_indexed(cmd.index_count, 1, 0, 0, 0);
    } else {
        vulkan_->draw(cmd.vertex_count > 0 ? cmd.vertex_count : cmd.index_count, 1, 0, 0);
    }
    
    draws_this_frame_++;
    
    LOGD("Draw: %s, %u %s", 
         cmd.indexed ? "indexed" : "non-indexed",
         cmd.indexed ? cmd.index_count : cmd.vertex_count,
         cmd.indexed ? "indices" : "vertices");
}

void CommandProcessor::on_register_write(u32 index, u32 value) {
    // Handle side effects of register writes
    
    switch (index) {
        case xenos_reg::CP_RB_WPTR:
            // Ring buffer write pointer updated - may need to process commands
            break;
            
        case xenos_reg::VGT_DRAW_INITIATOR:
            // Draw initiated via register write
            {
                DrawCommand cmd = {};
                cmd.primitive_type = static_cast<PrimitiveType>(value & 0x3F);
                cmd.indexed = (value >> 11) & 1;
                cmd.vertex_count = registers_[xenos_reg::VGT_IMMED_DATA];
                
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
