# Stream C: GPU Rendering Pipeline

**Priority**: MEDIUM (needed for visual output)  
**Estimated Time**: 12+ hours  
**Dependencies**: None (can start immediately)  
**Blocks**: No graphics display

## Overview

The emulator has substantial GPU infrastructure:

- `VulkanBackend` - 2300+ lines of Vulkan rendering code
- `ShaderTranslator` - 2000 lines of Xenos shader parsing
- `CommandProcessor` - Xenos command buffer parsing
- `SPIRV Builder` - SPIR-V code generation

These components exist but are not fully wired together. Your task is to connect them so games can render graphics.

## Files to Modify

- `native/src/gpu/xenos/command_processor.cpp` - Wire to Vulkan backend
- `native/src/gpu/xenos/command_processor.h` - Add backend reference
- `native/src/gpu/xenos/shader_translator.cpp` - Complete translation pipeline
- `native/src/gpu/vulkan/vulkan_backend.cpp` - Connect shader output
- `native/src/gpu/vulkan/swapchain.cpp` - Surface presentation

## Architecture

```
Xbox 360 Game
     ↓
Command Buffer (Ring Buffer)
     ↓
CommandProcessor (parses PM4 packets)
     ↓
     ├── Register Writes → GPU State
     ├── Draw Commands → VulkanBackend::draw()
     └── Shader References → ShaderTranslator
                                   ↓
                             SPIR-V Builder
                                   ↓
                             VkShaderModule
                                   ↓
                             Graphics Pipeline
                                   ↓
                             Swapchain Present
```

---

## Task C.1: Command Processor to Vulkan Connection

**File**: `native/src/gpu/xenos/command_processor.cpp`

### C.1.1: Add Vulkan Backend Reference

```cpp
// In header (command_processor.h):
class VulkanBackend;

class CommandProcessor {
private:
    VulkanBackend* vulkan_ = nullptr;
    // ... existing members

public:
    void set_vulkan_backend(VulkanBackend* backend) { vulkan_ = backend; }
};
```

### C.1.2: Implement Draw Dispatch

The Xenos GPU uses PM4 (Packet Manager 4) command packets. Find where draw commands are parsed and call Vulkan:

```cpp
void CommandProcessor::execute_packet_type3(u32 packet, u32* data) {
    u32 opcode = (packet >> 8) & 0xFF;
    u32 count = ((packet >> 16) & 0x3FFF) + 1;

    switch (opcode) {
        case PM4_DRAW_INDX: {
            // Parse draw parameters from data
            u32 viz_query = data[0];
            u32 info = data[1];
            u32 index_count = info & 0xFFFF;
            u32 prim_type = (info >> 16) & 0x3F;

            if (vulkan_) {
                vulkan_->set_primitive_type(translate_primitive(prim_type));
                vulkan_->draw(index_count, 1, 0, 0);
            }
            break;
        }

        case PM4_DRAW_INDX_2: {
            // Indexed draw
            u32 info = data[0];
            u32 index_count = info & 0xFFFF;
            u32 prim_type = (info >> 16) & 0x3F;
            // Index data follows in packet

            if (vulkan_) {
                vulkan_->set_primitive_type(translate_primitive(prim_type));
                vulkan_->draw_indexed(index_count, 1, 0, 0, 0);
            }
            break;
        }

        case PM4_SET_CONSTANT: {
            // GPU register/constant writes
            u32 offset = data[0] & 0xFFFF;
            for (u32 i = 1; i < count; i++) {
                set_register(offset + i - 1, data[i]);
            }
            break;
        }

        // ... other packet types
    }
}

VkPrimitiveTopology CommandProcessor::translate_primitive(u32 xenos_prim) {
    switch (xenos_prim) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 3: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        // Xenos has more types like RECT_LIST, QUAD_LIST
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
```

### C.1.3: PM4 Packet Definitions

```cpp
// Add to header or cpp:
enum PM4Opcode {
    PM4_NOP                 = 0x10,
    PM4_INDIRECT_BUFFER     = 0x3F,
    PM4_WAIT_REG_MEM        = 0x3C,
    PM4_REG_RMW             = 0x21,
    PM4_SET_CONSTANT        = 0x2D,
    PM4_LOAD_ALU_CONSTANT   = 0x2F,
    PM4_IM_LOAD             = 0x27,
    PM4_IM_LOAD_IMMEDIATE   = 0x2B,
    PM4_DRAW_INDX           = 0x22,
    PM4_DRAW_INDX_2         = 0x36,
    PM4_VIZ_QUERY           = 0x23,
    PM4_EVENT_WRITE         = 0x2E,
    PM4_EVENT_WRITE_EXT     = 0x3E,
};
```

---

## Task C.2: Shader Translation Pipeline

**File**: `native/src/gpu/xenos/shader_translator.cpp`

### C.2.1: Entry Point

```cpp
ShaderTranslation ShaderTranslator::translate(const u32* microcode,
                                               u32 dword_count,
                                               ShaderType type) {
    ShaderTranslation result;
    result.type = type;

    // Reset state
    reset();
    shader_type_ = type;

    // Parse Xenos microcode
    if (!parse_microcode(microcode, dword_count)) {
        result.error = "Failed to parse microcode";
        return result;
    }

    // Generate SPIR-V
    result.spirv = generate_spirv();
    if (result.spirv.empty()) {
        result.error = "Failed to generate SPIR-V";
        return result;
    }

    result.success = true;
    return result;
}
```

### C.2.2: Connect to SPIR-V Builder

```cpp
std::vector<u32> ShaderTranslator::generate_spirv() {
    SpirVBuilder builder;

    // Set up shader type
    builder.set_shader_type(shader_type_ == ShaderType::Vertex ?
                            SpvExecutionModelVertex :
                            SpvExecutionModelFragment);

    // Declare inputs/outputs based on parsed shader
    for (const auto& input : inputs_) {
        builder.declare_input(input.location, input.type);
    }

    for (const auto& output : outputs_) {
        builder.declare_output(output.location, output.type);
    }

    // Emit translated instructions
    for (const auto& inst : translated_instructions_) {
        emit_instruction(builder, inst);
    }

    return builder.finalize();
}
```

### C.2.3: Key Xenos Shader Instructions

Xenos uses a custom shader ISA. Key instruction types to handle:

```cpp
void ShaderTranslator::emit_instruction(SpirVBuilder& builder,
                                         const XenosInstruction& inst) {
    switch (inst.opcode) {
        case XenosOp::ADD:
            builder.emit_fadd(inst.dst, inst.src0, inst.src1);
            break;
        case XenosOp::MUL:
            builder.emit_fmul(inst.dst, inst.src0, inst.src1);
            break;
        case XenosOp::MAD:  // Multiply-add (very common)
            builder.emit_fma(inst.dst, inst.src0, inst.src1, inst.src2);
            break;
        case XenosOp::DOT4:
            builder.emit_dot4(inst.dst, inst.src0, inst.src1);
            break;
        case XenosOp::FETCH:  // Texture/vertex fetch
            emit_fetch(builder, inst);
            break;
        // ... more opcodes
    }
}
```

---

## Task C.3: Vulkan Pipeline Creation

**File**: `native/src/gpu/vulkan/vulkan_backend.cpp`

### C.3.1: Create Pipeline from Shaders

```cpp
VkPipeline VulkanBackend::create_graphics_pipeline(
    VkShaderModule vertex_shader,
    VkShaderModule fragment_shader,
    const PipelineState& state) {

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";

    // ... rest of pipeline creation
    // (vertex input, input assembly, viewport, rasterizer,
    //  multisampling, depth/stencil, color blending, layout)

    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    // ... fill in all other members

    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device_, pipeline_cache_, 1,
                              &pipeline_info, nullptr, &pipeline);
    return pipeline;
}
```

### C.3.2: Shader Caching

```cpp
class ShaderCache {
    std::unordered_map<u64, VkShaderModule> cache_;

public:
    VkShaderModule get_or_create(VulkanBackend* backend,
                                  const u32* microcode,
                                  u32 size,
                                  ShaderType type) {
        u64 hash = compute_hash(microcode, size);

        auto it = cache_.find(hash);
        if (it != cache_.end()) {
            return it->second;
        }

        // Translate and create
        auto translation = shader_translator_.translate(microcode, size, type);
        if (!translation.success) {
            return VK_NULL_HANDLE;
        }

        VkShaderModule module = backend->create_shader_module(translation.spirv);
        cache_[hash] = module;
        return module;
    }
};
```

---

## Task C.4: Swapchain and Presentation

**File**: `native/src/gpu/vulkan/swapchain.cpp`

### C.4.1: Android Surface Integration

```cpp
bool Swapchain::create_for_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;

    VkResult result = vkCreateAndroidSurfaceKHR(instance_, &surface_info,
                                                 nullptr, &surface_);
    if (result != VK_SUCCESS) {
        return false;
    }

    return create_swapchain();
}

void Swapchain::present() {
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished_semaphore_;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &current_image_index_;

    vkQueuePresentKHR(present_queue_, &present_info);
}
```

---

## Testing

Since GPU output requires the full Android app:

1. Build and deploy to device
2. Load a simple game or test ROM
3. Check for Vulkan validation errors in logcat:
   ```bash
   adb logcat | grep -E "(Vulkan|VK_|360mu)"
   ```

**Incremental testing**:

- First verify shaders compile without errors
- Then verify draw calls execute
- Finally verify frame presentation

## Reference Files

- `native/src/gpu/vulkan/vulkan_backend.h` - See existing Vulkan infrastructure
- `native/src/gpu/xenos/shader_translator.h` - See shader types and structures
- `native/src/gpu/xenos/gpu.h` - See GPU interface
- Xenia emulator source - Reference for Xenos GPU behavior

## Notes

- Xbox 360 uses EDRAM (embedded RAM) for render targets - may need special handling
- Xenos has a unique tiled rendering mode
- Shader microcode is different from modern GPU shaders - translation is complex
- Consider using dynamic rendering (VK_KHR_dynamic_rendering) for flexibility
