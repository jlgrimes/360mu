/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos GPU Command Processor Implementation
 */

#include "command_processor.h"
#include "../vulkan/vulkan_backend.h"
#include "../../memory/memory.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-gpu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[GPU] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[GPU ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...)
#endif

namespace x360mu {

CommandProcessor::CommandProcessor() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
}

CommandProcessor::~CommandProcessor() = default;

Status CommandProcessor::initialize(Memory* memory, VulkanBackend* vulkan,
                                   ShaderTranslator* shader_translator,
                                   TextureCache* texture_cache) {
    memory_ = memory;
    vulkan_ = vulkan;
    shader_translator_ = shader_translator;
    texture_cache_ = texture_cache;
    
    LOGI("Command processor initialized");
    return Status::Ok;
}

void CommandProcessor::shutdown() {
    LOGI("Command processor shutdown");
}

void CommandProcessor::reset() {
    registers_.fill(0);
    vertex_constants_.fill(0.0f);
    pixel_constants_.fill(0.0f);
    bool_constants_.fill(0);
    loop_constants_.fill(0);
    frame_complete_ = false;
    in_frame_ = false;
    packets_processed_ = 0;
    draws_this_frame_ = 0;
}

bool CommandProcessor::process(GuestAddr ring_base, u32 ring_size, u32& read_ptr, u32 write_ptr) {
    frame_complete_ = false;
    draws_this_frame_ = 0;
    
    // Process packets until we catch up with write pointer
    while (read_ptr != write_ptr) {
        GuestAddr packet_addr = ring_base + (read_ptr * 4);
        
        u32 packets_consumed = 0;
        execute_packet(packet_addr, packets_consumed);
        
        // Advance read pointer (wrap around ring buffer)
        read_ptr = (read_ptr + packets_consumed) % (ring_size / 4);
        packets_processed_++;
        
        // Check if frame was completed
        if (frame_complete_) {
            break;
        }
    }
    
    return frame_complete_;
}

u32 CommandProcessor::execute_packet(GuestAddr addr, u32& packets_consumed) {
    u32 header = read_cmd(addr);
    
    PacketType type = static_cast<PacketType>((header >> 30) & 0x3);
    
    switch (type) {
        case PacketType::Type0:
            execute_type0(header, addr + 4);
            // Count is in bits 15:0
            packets_consumed = 1 + (header & 0x3FFF) + 1;
            break;
            
        case PacketType::Type2:
            execute_type2(header);
            packets_consumed = 1;
            break;
            
        case PacketType::Type3:
            execute_type3(header, addr + 4);
            // Count is in bits 15:0
            packets_consumed = 1 + ((header >> 16) & 0x3FFF) + 1;
            break;
            
        default:
            LOGE("Unknown packet type: %d at 0x%08X", static_cast<int>(type), addr);
            packets_consumed = 1;
            break;
    }
    
    return packets_consumed;
}

void CommandProcessor::execute_type0(u32 header, GuestAddr data_addr) {
    // Type 0: Register write
    // bits 13:0 = base register index
    // bits 15:14 = reserved
    // bits 29:16 = count - 1
    
    u32 base_reg = header & 0x3FFF;
    u32 count = ((header >> 16) & 0x3FFF) + 1;
    
    for (u32 i = 0; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        write_register(base_reg + i, value);
    }
}

void CommandProcessor::execute_type2(u32 header) {
    // Type 2: NOP packet
    // Just skip it
}

void CommandProcessor::execute_type3(u32 header, GuestAddr data_addr) {
    // Type 3: IT_OPCODE packet
    // bits 7:0 = opcode
    // bits 15:8 = reserved
    // bits 29:16 = count - 1
    
    PM4Opcode opcode = static_cast<PM4Opcode>(header & 0xFF);
    u32 count = ((header >> 16) & 0x3FFF) + 1;
    
    switch (opcode) {
        case PM4Opcode::NOP:
            // Nothing to do
            break;
            
        case PM4Opcode::DRAW_INDX:
            handle_draw_indx(data_addr, count);
            break;
            
        case PM4Opcode::DRAW_INDX_2:
            handle_draw_indx_2(data_addr, count);
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
        case PM4Opcode::SET_SHADER_CONSTANTS:
            handle_set_constant(data_addr, count);
            break;
            
        case PM4Opcode::EVENT_WRITE:
        case PM4Opcode::EVENT_WRITE_SHD:
        case PM4Opcode::EVENT_WRITE_EXT:
            handle_event_write(data_addr, count);
            break;
            
        case PM4Opcode::MEM_WRITE:
            handle_mem_write(data_addr, count);
            break;
            
        case PM4Opcode::WAIT_REG_MEM:
            handle_wait_reg_mem(data_addr, count);
            break;
            
        case PM4Opcode::WAIT_FOR_IDLE:
            // GPU synchronization - no-op in emulator
            break;
            
        case PM4Opcode::INDIRECT_BUFFER:
        case PM4Opcode::INDIRECT_BUFFER_PFD:
            handle_indirect_buffer(data_addr, count);
            break;
            
        case PM4Opcode::INTERRUPT:
            // Signal frame complete
            frame_complete_ = true;
            break;
            
        case PM4Opcode::ME_INIT:
        case PM4Opcode::CP_INVALIDATE_STATE:
        case PM4Opcode::CONTEXT_UPDATE:
            // State management - mostly no-op
            break;
            
        default:
            LOGD("Unhandled PM4 opcode: 0x%02X", static_cast<u32>(opcode));
            break;
    }
}

void CommandProcessor::handle_draw_indx(GuestAddr data_addr, u32 count) {
    // DRAW_INDX packet format:
    // dword 0: VGT_DRAW_INITIATOR
    // dword 1: size / primitive type
    // dword 2: index base (for indexed draws)
    // dword 3: index size
    
    u32 dw0 = read_cmd(data_addr);
    u32 dw1 = read_cmd(data_addr + 4);
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((dw1 >> 8) & 0x3F);
    cmd.index_count = dw1 & 0xFFFF;
    cmd.indexed = (dw0 & 0x1) != 0;
    
    if (cmd.indexed && count >= 3) {
        cmd.index_base = read_cmd(data_addr + 8);
        u32 dw3 = read_cmd(data_addr + 12);
        cmd.index_size = ((dw3 >> 6) & 1) ? 4 : 2;
    }
    
    execute_draw(cmd);
}

void CommandProcessor::handle_draw_indx_2(GuestAddr data_addr, u32 count) {
    // DRAW_INDX_2: Immediate indices in packet
    u32 dw0 = read_cmd(data_addr);
    
    DrawCommand cmd = {};
    cmd.primitive_type = static_cast<PrimitiveType>((dw0 >> 8) & 0x3F);
    cmd.index_count = dw0 & 0xFFFF;
    cmd.indexed = true;
    cmd.index_base = data_addr + 4;  // Indices follow in packet
    cmd.index_size = 2;  // Always 16-bit for immediate
    
    execute_draw(cmd);
}

void CommandProcessor::handle_load_alu_constant(GuestAddr data_addr, u32 count) {
    // Load shader constants from memory
    u32 dw0 = read_cmd(data_addr);
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    u32 start_reg = dw0 & 0x1FF;
    u32 num_regs = (dw0 >> 16) & 0x1FF;
    bool vertex = (dw0 >> 31) == 0;
    
    f32* dest = vertex ? vertex_constants_.data() : pixel_constants_.data();
    
    for (u32 i = 0; i < num_regs * 4; i++) {
        u32 raw = memory_->read_u32(src_addr + i * 4);
        dest[(start_reg * 4) + i] = *reinterpret_cast<f32*>(&raw);
    }
}

void CommandProcessor::handle_load_bool_constant(GuestAddr data_addr, u32 count) {
    u32 dw0 = read_cmd(data_addr);
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    u32 start_reg = dw0 & 0xFF;
    u32 num_regs = ((dw0 >> 16) & 0xFF) + 1;
    
    for (u32 i = 0; i < num_regs; i++) {
        bool_constants_[start_reg + i] = memory_->read_u32(src_addr + i * 4);
    }
}

void CommandProcessor::handle_load_loop_constant(GuestAddr data_addr, u32 count) {
    u32 dw0 = read_cmd(data_addr);
    GuestAddr src_addr = read_cmd(data_addr + 4);
    
    u32 start_reg = dw0 & 0x1F;
    u32 num_regs = ((dw0 >> 16) & 0x1F) + 1;
    
    for (u32 i = 0; i < num_regs; i++) {
        loop_constants_[start_reg + i] = memory_->read_u32(src_addr + i * 4);
    }
}

void CommandProcessor::handle_set_constant(GuestAddr data_addr, u32 count) {
    // SET_CONSTANT: Write shader constants directly
    u32 dw0 = read_cmd(data_addr);
    
    u32 const_offset = dw0 & 0xFFFF;
    
    // Determine target based on offset range
    // 0x000-0x0FF: Vertex shader constants
    // 0x100-0x1FF: Pixel shader constants
    // 0x200-0x2FF: Fetch constants
    
    for (u32 i = 1; i < count; i++) {
        u32 value = read_cmd(data_addr + i * 4);
        u32 reg = const_offset + (i - 1);
        
        if (reg < 0x100) {
            // Vertex constant
            u32 raw = value;
            vertex_constants_[reg * 4] = *reinterpret_cast<f32*>(&raw);
        } else if (reg < 0x200) {
            // Pixel constant
            u32 raw = value;
            pixel_constants_[(reg - 0x100) * 4] = *reinterpret_cast<f32*>(&raw);
        } else if (reg < 0x300) {
            // Fetch constant
            u32 fetch_idx = (reg - 0x200) / 6;
            u32 fetch_word = (reg - 0x200) % 6;
            if (fetch_idx < 96) {
                vertex_fetch_[fetch_idx].data[fetch_word] = value;
            }
        }
    }
}

void CommandProcessor::handle_event_write(GuestAddr data_addr, u32 count) {
    u32 dw0 = read_cmd(data_addr);
    u32 event = dw0 & 0xFF;
    
    // Event types:
    // 0x16 = VS_DONE
    // 0x17 = PS_DONE
    // 0x2B = CACHE_FLUSH_TS (texture cache flush with timestamp)
    // 0x2C = CONTEXT_DONE
    
    switch (event) {
        case 0x16: // VS_DONE
        case 0x17: // PS_DONE
        case 0x2C: // CONTEXT_DONE
            // Synchronization events - mostly handled implicitly
            break;
            
        case 0x2B: // CACHE_FLUSH_TS
            // Flush texture cache and write timestamp
            if (count >= 3) {
                GuestAddr addr = read_cmd(data_addr + 4);
                u32 timestamp = read_cmd(data_addr + 8);
                memory_->write_u32(static_cast<GuestAddr>(addr), timestamp);
            }
            break;
    }
    
    // Check for frame-ending events
    if (event == 0x17 || event == 0x2C) {
        // Potentially end of frame
        // Check if we should signal frame complete based on render target state
    }
}

void CommandProcessor::handle_mem_write(GuestAddr data_addr, u32 count) {
    // Write value to memory address
    GuestAddr addr = read_cmd(data_addr);
    u32 value = read_cmd(data_addr + 4);
    
    memory_->write_u32(static_cast<GuestAddr>(addr), value);
}

void CommandProcessor::handle_wait_reg_mem(GuestAddr data_addr, u32 count) {
    // Wait for register/memory value
    // In emulation, we generally don't need to actually wait
    // since we execute sequentially
}

void CommandProcessor::handle_indirect_buffer(GuestAddr data_addr, u32 count) {
    // Execute commands from indirect buffer
    GuestAddr ib_addr = read_cmd(data_addr);
    u32 ib_size = read_cmd(data_addr + 4) & 0xFFFFF;  // Size in dwords
    
    // Process the indirect buffer
    u32 read_ptr = 0;
    while (read_ptr < ib_size) {
        u32 packets_consumed = 0;
        execute_packet(ib_addr + read_ptr * 4, packets_consumed);
        read_ptr += packets_consumed;
    }
}

void CommandProcessor::set_register(u32 index, u32 value) {
    if (index < registers_.size()) {
        registers_[index] = value;
    }
}

void CommandProcessor::write_register(u32 index, u32 value) {
    if (index >= registers_.size()) {
        return;
    }
    
    registers_[index] = value;
    on_register_write(index, value);
}

void CommandProcessor::on_register_write(u32 index, u32 value) {
    // Handle register write side effects
    using namespace xenos_reg;
    
    switch (index) {
        case RB_SURFACE_INFO:
            render_state_.color_pitch = value & 0x3FFF;
            break;
            
        case RB_COLOR_INFO:
            render_state_.color_target_address = value & 0xFFFFF000;
            render_state_.color_format = static_cast<SurfaceFormat>((value >> 0) & 0xF);
            break;
            
        case RB_DEPTH_INFO:
            render_state_.depth_target_address = value & 0xFFFFF000;
            break;
            
        case PA_CL_VPORT_XSCALE:
            render_state_.viewport_width = *reinterpret_cast<f32*>(&value) * 2.0f;
            break;
            
        case PA_CL_VPORT_YSCALE:
            render_state_.viewport_height = *reinterpret_cast<f32*>(&value) * -2.0f;
            break;
            
        case PA_CL_VPORT_XOFFSET:
            render_state_.viewport_x = *reinterpret_cast<f32*>(&value) - render_state_.viewport_width / 2.0f;
            break;
            
        case PA_CL_VPORT_YOFFSET:
            render_state_.viewport_y = *reinterpret_cast<f32*>(&value) - render_state_.viewport_height / 2.0f;
            break;
            
        case PA_SC_SCREEN_SCISSOR_TL:
            render_state_.scissor_left = value & 0x7FFF;
            render_state_.scissor_top = (value >> 16) & 0x7FFF;
            break;
            
        case PA_SC_SCREEN_SCISSOR_BR:
            render_state_.scissor_right = value & 0x7FFF;
            render_state_.scissor_bottom = (value >> 16) & 0x7FFF;
            break;
            
        case SQ_VS_PROGRAM:
            render_state_.vertex_shader_address = value;
            break;
            
        case SQ_PS_PROGRAM:
            render_state_.pixel_shader_address = value;
            break;
    }
}

void CommandProcessor::update_render_state() {
    // Copy fetch constants to render state
    for (u32 i = 0; i < 96; i++) {
        render_state_.vertex_fetch[i] = vertex_fetch_[i];
    }
    for (u32 i = 0; i < 32; i++) {
        render_state_.texture_fetch[i] = texture_fetch_[i];
    }
}

void CommandProcessor::execute_draw(const DrawCommand& cmd) {
    // Update state before draw
    update_render_state();
    update_shaders();
    update_textures();
    
    LOGD("Draw: primitive=%d, count=%d, indexed=%d", 
         static_cast<int>(cmd.primitive_type), cmd.index_count, cmd.indexed);
    
    if (!vulkan_) {
        return;
    }
    
    // TODO: Actually execute the draw call through Vulkan
    // This requires:
    // 1. Translate shaders if not cached
    // 2. Create/bind pipeline
    // 3. Bind vertex/index buffers
    // 4. Bind descriptors (textures, constants)
    // 5. Issue draw command
    
    draws_this_frame_++;
}

void CommandProcessor::update_shaders() {
    // Translate vertex shader if needed
    if (render_state_.vertex_shader_address && shader_translator_) {
        // Read shader microcode from memory
        // void* vs_code = memory_->get_host_ptr(render_state_.vertex_shader_address);
        // shader_translator_->translate(vs_code, size, ShaderType::Vertex);
    }
    
    // Translate pixel shader if needed
    if (render_state_.pixel_shader_address && shader_translator_) {
        // void* ps_code = memory_->get_host_ptr(render_state_.pixel_shader_address);
        // shader_translator_->translate(ps_code, size, ShaderType::Pixel);
    }
}

void CommandProcessor::update_textures() {
    // Process texture fetch constants and load textures
    for (u32 i = 0; i < 32; i++) {
        const auto& fetch = texture_fetch_[i];
        if (fetch.data[0] == 0) continue;  // Not configured
        
        if (texture_cache_) {
            // texture_cache_->get_texture(fetch, memory_);
        }
    }
}

// ShaderMicrocode implementation
Status ShaderMicrocode::parse(const void* data, u32 size, ShaderType type) {
    type_ = type;
    
    if (size < 16) {
        return Status::InvalidFormat;
    }
    
    const u32* words = static_cast<const u32*>(data);
    u32 dword_count = size / 4;
    
    // Copy raw instructions
    instructions_.assign(words, words + dword_count);
    
    // Decode control flow instructions
    decode_control_flow();
    
    return Status::Ok;
}

void ShaderMicrocode::decode_control_flow() {
    // Xenos shaders start with control flow instructions
    // Each CF instruction is 48 bits (1.5 dwords)
    
    u32 cf_offset = 0;
    bool end_found = false;
    
    while (!end_found && cf_offset < instructions_.size()) {
        ControlFlowInstruction cf;
        
        // Read 48-bit instruction
        u32 lo = instructions_[cf_offset];
        u32 hi = cf_offset + 1 < instructions_.size() ? instructions_[cf_offset + 1] : 0;
        
        cf.word = lo;
        cf.opcode = (lo >> 23) & 0x1F;
        cf.address = lo & 0x1FF;
        cf.count = (lo >> 10) & 0x7;
        cf.end_of_shader = (lo >> 20) & 1;
        cf.predicated = (lo >> 21) & 1;
        cf.condition = (lo >> 22) & 1;
        
        cf_instructions_.push_back(cf);
        
        // Decode referenced ALU or fetch clauses
        if (cf.opcode >= 0 && cf.opcode <= 3) {
            // EXEC instruction - execute ALU clause
            decode_alu_clause(cf.address, cf.count + 1);
        } else if (cf.opcode >= 4 && cf.opcode <= 7) {
            // EXEC with fetch
            decode_fetch_clause(cf.address, cf.count + 1);
        }
        
        if (cf.end_of_shader) {
            end_found = true;
        }
        
        cf_offset += 2;  // 48 bits = 1.5 dwords, but aligned to 2
    }
}

void ShaderMicrocode::decode_alu_clause(u32 address, u32 count) {
    // ALU instructions are 96 bits each (3 dwords)
    for (u32 i = 0; i < count; i++) {
        u32 offset = (address + i) * 3;
        if (offset + 2 >= instructions_.size()) break;
        
        AluInstruction alu;
        alu.words[0] = instructions_[offset];
        alu.words[1] = instructions_[offset + 1];
        alu.words[2] = instructions_[offset + 2];
        
        // Decode ALU fields (simplified)
        alu.vector_opcode = (alu.words[0] >> 0) & 0x1F;
        alu.scalar_opcode = (alu.words[0] >> 5) & 0x3F;
        alu.dest_reg = (alu.words[1] >> 24) & 0x7F;
        alu.write_mask = (alu.words[1] >> 20) & 0xF;
        alu.export_data = (alu.words[1] >> 31) & 1;
        
        alu_instructions_.push_back(alu);
    }
}

void ShaderMicrocode::decode_fetch_clause(u32 address, u32 count) {
    // Fetch instructions are 96 bits each (3 dwords)
    for (u32 i = 0; i < count; i++) {
        u32 offset = (address + i) * 3;
        if (offset + 2 >= instructions_.size()) break;
        
        FetchInstruction fetch;
        fetch.words[0] = instructions_[offset];
        fetch.words[1] = instructions_[offset + 1];
        fetch.words[2] = instructions_[offset + 2];
        
        // Decode fetch fields
        fetch.opcode = (fetch.words[0] >> 0) & 0x1F;
        fetch.const_index = (fetch.words[0] >> 12) & 0x1F;
        fetch.dest_reg = (fetch.words[1] >> 16) & 0x7F;
        fetch.src_reg = (fetch.words[1] >> 9) & 0x7F;
        fetch.fetch_type = (fetch.words[0] >> 5) & 0x3;
        
        fetch_instructions_.push_back(fetch);
    }
}

} // namespace x360mu

