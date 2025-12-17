# Task: GPU Command Processor

## Priority: 🔴 CRITICAL (Blocking)
## Estimated Time: 3-4 weeks
## Dependencies: TASK_02_VULKAN_BACKEND

---

## Objective

Parse Xbox 360 GPU command buffers (PM4 packets) and dispatch rendering operations.

---

## What To Build

### Location
- `native/src/gpu/xenos/command_processor.cpp`
- `native/src/gpu/xenos/command_processor.h`

---

## Background: PM4 Command Format

The Xenos GPU uses PM4 (Packet Manager 4) format from ATI:
- Commands are 32-bit words
- First word is header with type and count
- Followed by data words

---

## Specific Implementation

### 1. Packet Types

```cpp
enum class PM4Type : u32 {
    Type0 = 0,  // Register write
    Type1 = 1,  // Reserved
    Type2 = 2,  // Reserved  
    Type3 = 3,  // Command packet
};

// Type 3 opcodes
enum class PM4Opcode : u32 {
    NOP = 0x10,
    SET_CONSTANT = 0x2D,
    SET_SHADER_CONSTANTS = 0x44,
    LOAD_ALU_CONSTANT = 0x18,
    
    DRAW_INDEX = 0x22,
    DRAW_INDEX_AUTO = 0x24,
    DRAW_INDEX_IMMD = 0x29,
    VIZ_QUERY = 0x23,
    
    SET_BIN_MASK_LO = 0x60,
    SET_BIN_SELECT_LO = 0x61,
    
    WAIT_FOR_IDLE = 0x26,
    WAIT_REG_MEM = 0x3C,
    REG_RMW = 0x21,
    
    COND_WRITE = 0x45,
    EVENT_WRITE = 0x46,
    EVENT_WRITE_EXT = 0x59,
    
    INDIRECT_BUFFER = 0x3F,
    MEM_WRITE = 0x3D,
    INVALIDATE_STATE = 0x3B,
};
```

### 2. Packet Parser

```cpp
class CommandProcessor {
public:
    void process_ring_buffer(const u32* commands, size_t count);
    
private:
    void process_packet(const u32* packet);
    
    // Type 0 - Register writes
    void process_type0(u32 header, const u32* data);
    
    // Type 3 - Commands
    void process_type3(u32 header, const u32* data);
    
    // Specific command handlers
    void cmd_draw_index(const u32* data, u32 count);
    void cmd_draw_index_auto(const u32* data, u32 count);
    void cmd_set_constant(const u32* data, u32 count);
    void cmd_wait_for_idle(const u32* data, u32 count);
    void cmd_event_write(const u32* data, u32 count);
    void cmd_indirect_buffer(const u32* data, u32 count);
    
    // Current GPU state
    GpuState state_;
};
```

### 3. Packet Parsing

```cpp
void CommandProcessor::process_ring_buffer(const u32* commands, size_t count) {
    const u32* ptr = commands;
    const u32* end = commands + count;
    
    while (ptr < end) {
        u32 header = *ptr++;
        
        // Extract type from bits 31:30
        PM4Type type = static_cast<PM4Type>((header >> 30) & 0x3);
        
        switch (type) {
            case PM4Type::Type0: {
                // Register write: bits 15:0 = base reg, bits 29:16 = count
                u32 base_reg = header & 0xFFFF;
                u32 count = ((header >> 16) & 0x3FFF) + 1;
                process_type0_write(base_reg, ptr, count);
                ptr += count;
                break;
            }
            
            case PM4Type::Type3: {
                // Command: bits 7:0 = opcode, bits 29:16 = count
                PM4Opcode opcode = static_cast<PM4Opcode>((header >> 8) & 0xFF);
                u32 count = ((header >> 16) & 0x3FFF) + 1;
                process_type3_command(opcode, ptr, count);
                ptr += count;
                break;
            }
            
            case PM4Type::Type2:
                // NOP - skip
                break;
        }
    }
}
```

### 4. Draw Command Handling

```cpp
void CommandProcessor::cmd_draw_index(const u32* data, u32 count) {
    // data[0] = VGT_DMA_BASE (index buffer address)
    // data[1] = VGT_DMA_SIZE (index count << 16 | index type)
    
    u32 index_base = data[0];
    u32 index_count = data[1] >> 16;
    u32 index_type = data[1] & 0xFFFF;  // 0=u16, 1=u32
    
    // data[2] = VGT_DRAW_INITIATOR
    u32 prim_type = data[2] & 0x3F;
    
    DrawInfo draw;
    draw.index_buffer = memory_->translate(index_base);
    draw.index_count = index_count;
    draw.index_format = (index_type == 0) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    draw.topology = translate_primitive_type(prim_type);
    
    emit_draw(draw);
}

void CommandProcessor::cmd_draw_index_auto(const u32* data, u32 count) {
    // Non-indexed draw
    u32 vertex_count = data[0];
    u32 prim_type = data[1] & 0x3F;
    
    DrawInfo draw;
    draw.vertex_count = vertex_count;
    draw.topology = translate_primitive_type(prim_type);
    
    emit_draw_auto(draw);
}
```

### 5. Register State

```cpp
struct GpuState {
    // Shader state
    u32 vertex_shader_addr;
    u32 pixel_shader_addr;
    
    // Vertex format
    u32 vertex_fetch_constants[96];
    
    // Render targets
    u32 rb_color_info[4];
    u32 rb_depth_info;
    u32 rb_surface_info;
    
    // Viewport
    float viewport_scale[4];
    float viewport_offset[4];
    
    // Rasterizer
    u32 pa_su_sc_mode_cntl;
    u32 pa_cl_clip_cntl;
    
    // Constants
    float alu_constants[256 * 4];
    u32 bool_constants[8];
    u32 loop_constants[32];
};

void CommandProcessor::process_type0_write(u32 base_reg, const u32* data, u32 count) {
    for (u32 i = 0; i < count; i++) {
        write_register(base_reg + i, data[i]);
    }
}

void CommandProcessor::write_register(u32 reg, u32 value) {
    switch (reg) {
        case 0x2180: state_.vertex_shader_addr = value; break;
        case 0x2184: state_.pixel_shader_addr = value; break;
        // ... many more registers
    }
}
```

### 6. Primitive Types

```cpp
VkPrimitiveTopology translate_primitive_type(u32 type) {
    switch (type) {
        case 0x00: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 0x01: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 0x03: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 0x04: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case 0x11: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
```

---

## GPU Register Map (Key Registers)

| Address | Name | Description |
|---------|------|-------------|
| 0x2180 | SQ_VS_PROGRAM | Vertex shader address |
| 0x2184 | SQ_PS_PROGRAM | Pixel shader address |
| 0x2000-0x20FF | FETCH_CONSTANTS | Vertex buffer bindings |
| 0x4000-0x43FF | ALU_CONSTANTS | Shader constants |
| 0x2100 | RB_COLOR_INFO | Render target format |
| 0x2104 | RB_DEPTH_INFO | Depth buffer format |

---

## Test Cases

```cpp
TEST(CommandProcessorTest, ParseType0) {
    CommandProcessor cp;
    
    // Type 0 packet: write 2 registers starting at 0x2180
    u32 commands[] = {
        0x00012180,  // Header: type0, count=1, base=0x2180
        0x12340000,  // vertex shader addr
        0x56780000,  // pixel shader addr
    };
    
    cp.process_ring_buffer(commands, 3);
    EXPECT_EQ(cp.get_state().vertex_shader_addr, 0x12340000);
}

TEST(CommandProcessorTest, DrawIndexed) {
    CommandProcessor cp;
    MockGpu gpu;
    cp.set_gpu(&gpu);
    
    u32 commands[] = {
        0xC0022200,  // Type3, opcode=DRAW_INDEX, count=2
        0x00100000,  // index buffer addr
        0x00030000,  // 3 indices, u16 format
        0x00000003,  // triangle list
    };
    
    cp.process_ring_buffer(commands, 4);
    EXPECT_EQ(gpu.last_draw().index_count, 3);
}
```

---

## Do NOT Touch

- Shader translation (separate task)
- Vulkan backend (separate task)
- eDRAM/texture handling (separate task)
- CPU code

---

## Success Criteria

1. ✅ Parse Type 0 (register write) packets
2. ✅ Parse Type 3 (command) packets
3. ✅ Handle DRAW_INDEX command
4. ✅ Handle SET_CONSTANT command
5. ✅ Track GPU register state

---

*This task focuses only on command parsing. Shader translation and Vulkan rendering are separate.*

