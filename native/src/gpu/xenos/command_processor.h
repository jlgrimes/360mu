/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos GPU Command Processor
 * Parses and executes GPU command packets from the ring buffer
 */

#pragma once

#include "x360mu/types.h"
#include "gpu.h"
#include <vector>
#include <functional>

namespace x360mu {

class Memory;
class VulkanBackend;
class ShaderTranslator;
class TextureCache;

/**
 * GPU packet types
 */
enum class PacketType : u32 {
    Type0 = 0,  // Register write
    Type1 = 1,  // Reserved
    Type2 = 2,  // NOP
    Type3 = 3,  // Command packet
};

/**
 * Type 3 opcodes
 */
enum class PM4Opcode : u32 {
    NOP = 0x10,
    INTERRUPT = 0x40,
    
    // Waits
    WAIT_FOR_IDLE = 0x26,
    WAIT_REG_MEM = 0x3C,
    
    // Register operations
    REG_RMW = 0x21,
    LOAD_ALU_CONSTANT = 0x2F,
    LOAD_BOOL_CONSTANT = 0x2E,
    LOAD_LOOP_CONSTANT = 0x30,
    SET_CONSTANT = 0x2D,
    SET_CONSTANT2 = 0x55,
    SET_SHADER_CONSTANTS = 0x56,
    
    // Drawing
    DRAW_INDX = 0x22,
    DRAW_INDX_2 = 0x36,
    DRAW_INDX_BIN = 0x35,
    DRAW_INDX_IMMD = 0x2A,
    VIZ_QUERY = 0x23,
    
    // Memory
    MEM_WRITE = 0x3D,
    COND_WRITE = 0x45,
    EVENT_WRITE = 0x46,
    EVENT_WRITE_SHD = 0x58,
    EVENT_WRITE_EXT = 0x59,
    
    // Binning
    SET_BIN_SELECT_LO = 0x60,
    SET_BIN_SELECT_HI = 0x61,
    SET_BIN_MASK_LO = 0x64,
    SET_BIN_MASK_HI = 0x65,
    
    // Context
    CONTEXT_UPDATE = 0x5E,
    
    // Synchronization
    ME_INIT = 0x48,
    CP_INVALIDATE_STATE = 0x3B,
    
    // Indirect buffer
    INDIRECT_BUFFER = 0x3F,
    INDIRECT_BUFFER_PFD = 0x37,
};

/**
 * Draw info extracted from command
 */
struct DrawCommand {
    PrimitiveType primitive_type;
    u32 index_count;
    GuestAddr index_base;
    u32 index_size;  // 2 or 4 bytes
    u32 vertex_count;
    bool indexed;
    u32 base_vertex;
    u32 start_index;
};

/**
 * Command processor handles GPU packet parsing
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
                     TextureCache* texture_cache);
    
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
     * Get register value
     */
    u32 get_register(u32 index) const { return registers_[index]; }
    
    /**
     * Set register value
     */
    void set_register(u32 index, u32 value);
    
    /**
     * Write register (with side effects)
     */
    void write_register(u32 index, u32 value);
    
    /**
     * Get current render state
     */
    const RenderState& render_state() const { return render_state_; }
    
    /**
     * Frame complete flag
     */
    bool frame_complete() const { return frame_complete_; }
    void clear_frame_complete() { frame_complete_ = false; }
    
private:
    Memory* memory_ = nullptr;
    VulkanBackend* vulkan_ = nullptr;
    ShaderTranslator* shader_translator_ = nullptr;
    TextureCache* texture_cache_ = nullptr;
    
    // Registers
    std::array<u32, 0x10000> registers_;
    
    // Current render state
    RenderState render_state_;
    
    // Shader constants
    std::array<f32, 256 * 4> vertex_constants_;   // 256 float4 constants
    std::array<f32, 256 * 4> pixel_constants_;
    std::array<u32, 256> bool_constants_;
    std::array<u32, 32> loop_constants_;
    
    // Fetch constants (vertex buffers + textures)
    std::array<FetchConstant, 96> vertex_fetch_;
    std::array<FetchConstant, 32> texture_fetch_;
    
    // Frame state
    bool frame_complete_ = false;
    bool in_frame_ = false;
    
    // Stats
    u64 packets_processed_ = 0;
    u64 draws_this_frame_ = 0;
    
    // Packet processing
    u32 execute_packet(GuestAddr addr, u32& packets_consumed);
    void execute_type0(u32 header, GuestAddr data_addr);
    void execute_type2(u32 header);
    void execute_type3(u32 header, GuestAddr data_addr);
    
    // Type 3 handlers
    void handle_draw_indx(GuestAddr data_addr, u32 count);
    void handle_draw_indx_2(GuestAddr data_addr, u32 count);
    void handle_load_alu_constant(GuestAddr data_addr, u32 count);
    void handle_load_bool_constant(GuestAddr data_addr, u32 count);
    void handle_load_loop_constant(GuestAddr data_addr, u32 count);
    void handle_set_constant(GuestAddr data_addr, u32 count);
    void handle_event_write(GuestAddr data_addr, u32 count);
    void handle_mem_write(GuestAddr data_addr, u32 count);
    void handle_wait_reg_mem(GuestAddr data_addr, u32 count);
    void handle_indirect_buffer(GuestAddr data_addr, u32 count);
    
    // State update
    void update_render_state();
    void update_shaders();
    void update_textures();
    void update_vertex_buffers();
    
    // Draw execution
    void execute_draw(const DrawCommand& cmd);
    
    // Register side effects
    void on_register_write(u32 index, u32 value);
    
    // Helper to read command buffer
    u32 read_cmd(GuestAddr addr) { return memory_->read_u32(addr); }
};

/**
 * Xenos shader microcode parser
 */
class ShaderMicrocode {
public:
    /**
     * Parse shader from memory
     */
    Status parse(const void* data, u32 size, ShaderType type);
    
    /**
     * Get shader type
     */
    ShaderType type() const { return type_; }
    
    /**
     * Get instruction count
     */
    u32 instruction_count() const { return instructions_.size(); }
    
    /**
     * ALU instruction encoding
     */
    struct AluInstruction {
        u32 words[3];  // 96-bit instruction
        
        // Decoded fields
        u8 scalar_opcode;
        u8 vector_opcode;
        u8 dest_reg;
        u8 src_regs[3];
        bool abs[3];
        bool negate[3];
        u8 write_mask;
        bool export_data;
        u8 export_type;
    };
    
    /**
     * Fetch instruction encoding
     */
    struct FetchInstruction {
        u32 words[3];  // 96-bit instruction
        
        // Decoded fields
        u8 opcode;
        u8 dest_reg;
        u8 src_reg;
        u8 const_index;
        u8 fetch_type;  // Vertex or texture
        u32 offset;
        u8 data_format;
        bool signed_rf;
        u8 num_format;
        u8 stride;
    };
    
    /**
     * Control flow instruction
     */
    struct ControlFlowInstruction {
        u32 word;
        
        u8 opcode;
        u16 address;
        u8 count;
        bool end_of_shader;
        bool predicated;
        bool condition;
    };
    
    // Access decoded instructions
    const std::vector<ControlFlowInstruction>& cf_instructions() const { return cf_instructions_; }
    const std::vector<AluInstruction>& alu_instructions() const { return alu_instructions_; }
    const std::vector<FetchInstruction>& fetch_instructions() const { return fetch_instructions_; }
    
private:
    ShaderType type_;
    std::vector<ControlFlowInstruction> cf_instructions_;
    std::vector<AluInstruction> alu_instructions_;
    std::vector<FetchInstruction> fetch_instructions_;
    std::vector<u32> instructions_;
    
    void decode_control_flow();
    void decode_alu_clause(u32 address, u32 count);
    void decode_fetch_clause(u32 address, u32 count);
};

} // namespace x360mu

