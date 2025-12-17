# Task: Xenos Shader Translator

## Priority: 🔴 CRITICAL (Blocking)
## Estimated Time: 6-10 weeks
## Dependencies: TASK_02_VULKAN_BACKEND (needs Vulkan device)

---

## Objective

Translate Xbox 360 Xenos GPU shader microcode into SPIR-V for Vulkan execution.

---

## What To Build

### Location
- `native/src/gpu/xenos/shader_translator.cpp`
- `native/src/gpu/xenos/shader_translator.h`

---

## Background: Xenos Shader Format

Xbox 360 shaders are in a proprietary microcode format:
- **Vertex shaders**: Transform vertices, output to parameter cache
- **Pixel shaders**: Sample textures, compute color output
- Uses ALU (Arithmetic Logic Unit) and TEX (Texture) operations
- Registers: r0-r127 (temp), c0-c255 (constants), t0-t15 (textures)

---

## Specific Implementation

### 1. Shader Binary Parsing

```cpp
struct XenosShaderInfo {
    enum Type { Vertex, Pixel };
    Type type;
    u32 instruction_count;
    std::vector<u32> microcode;  // Raw microcode dwords
    
    // Register usage
    u32 temp_register_count;
    u32 const_register_count;
    std::vector<u32> texture_bindings;
    
    // Input/output
    std::vector<VertexAttribute> inputs;  // For vertex shader
    std::vector<Interpolant> outputs;     // Varying outputs
};

// Parse shader from memory
XenosShaderInfo parse_shader(const u32* data, size_t size);
```

### 2. Instruction Decoding

Xenos instructions are 96 bits (3 dwords):

```cpp
struct XenosAluInst {
    // Dword 0
    u32 vector_dest : 6;
    u32 vector_dest_rel : 1;
    u32 scalar_dest : 6;
    u32 scalar_dest_rel : 1;
    u32 export_data : 1;
    // ... more fields
    
    // Dword 1 - Vector operation
    u32 vector_opcode : 5;
    u32 src1_reg : 8;
    u32 src1_sel : 2;
    u32 src1_swiz : 8;
    u32 src1_neg : 1;
    // ... more fields
    
    // Dword 2 - Scalar operation
    u32 scalar_opcode : 5;
    u32 src3_reg : 8;
    // ...
};

// ALU opcodes
enum class AluVectorOp : u32 {
    ADD = 0,
    MUL = 1,
    MAX = 2,
    MIN = 3,
    DOT4 = 4,
    DOT3 = 5,
    // ... ~30 more
};

enum class AluScalarOp : u32 {
    ADDS = 0,
    MULS = 1,
    MAXS = 2,
    MINS = 3,
    RECIP = 4,
    RSQRT = 5,
    EXP = 6,
    LOG = 7,
    // ... ~20 more
};
```

### 3. SPIR-V Builder

```cpp
class SpirvBuilder {
public:
    void begin_function(const char* name, bool is_vertex);
    void end_function();
    
    // Declare inputs/outputs
    u32 declare_input(u32 location, VkFormat format);
    u32 declare_output(u32 location, VkFormat format);
    u32 declare_uniform_buffer(u32 binding, u32 size);
    u32 declare_sampler(u32 binding);
    
    // ALU operations
    u32 emit_add(u32 a, u32 b);
    u32 emit_mul(u32 a, u32 b);
    u32 emit_dot3(u32 a, u32 b);
    u32 emit_dot4(u32 a, u32 b);
    u32 emit_rsqrt(u32 x);
    u32 emit_exp(u32 x);
    u32 emit_log(u32 x);
    u32 emit_frac(u32 x);
    u32 emit_floor(u32 x);
    
    // Texture operations
    u32 emit_sample(u32 sampler, u32 coord);
    u32 emit_sample_lod(u32 sampler, u32 coord, u32 lod);
    
    // Control flow
    void emit_if(u32 condition);
    void emit_else();
    void emit_endif();
    void emit_loop_begin();
    void emit_loop_end();
    
    // Get final SPIR-V binary
    std::vector<u32> finalize();
    
private:
    std::vector<u32> spirv_;
    u32 next_id_ = 1;
    
    u32 alloc_id() { return next_id_++; }
};
```

### 4. Main Translation Function

```cpp
class ShaderTranslator {
public:
    struct TranslatedShader {
        std::vector<u32> spirv;
        VkShaderModule module;
        u64 hash;  // For caching
    };
    
    TranslatedShader translate(const XenosShaderInfo& shader);
    
private:
    void translate_alu_instruction(const XenosAluInst& inst);
    void translate_tex_instruction(const XenosTexInst& inst);
    void translate_cf_instruction(const XenosCfInst& inst);
    
    // Handle swizzles (e.g., .xyz, .xxxx, .wzyx)
    u32 apply_swizzle(u32 value, u32 swizzle);
    
    // Handle destination write mask
    void write_with_mask(u32 dest, u32 value, u32 mask);
    
    SpirvBuilder builder_;
};
```

### 5. Shader Cache

```cpp
class ShaderCache {
public:
    VkShaderModule get_or_create(const u32* microcode, size_t size, bool is_vertex);
    
    void save_to_disk(const std::string& path);
    void load_from_disk(const std::string& path);
    
private:
    std::unordered_map<u64, VkShaderModule> cache_;
    
    u64 hash_shader(const u32* data, size_t size);
};
```

---

## Translation Examples

### Simple Vertex Shader

Xenos microcode for position transform:
```
// Multiply position by MVP matrix
dp4 oPos.x, r0, c0
dp4 oPos.y, r0, c1
dp4 oPos.z, r0, c2
dp4 oPos.w, r0, c3
```

Translates to SPIR-V:
```glsl
// GLSL equivalent (SPIR-V generated)
void main() {
    gl_Position.x = dot(inPosition, ubo.mvp[0]);
    gl_Position.y = dot(inPosition, ubo.mvp[1]);
    gl_Position.z = dot(inPosition, ubo.mvp[2]);
    gl_Position.w = dot(inPosition, ubo.mvp[3]);
}
```

### Simple Pixel Shader

Xenos:
```
// Sample texture and output
tfetch2D r0, r0.xy, t0
mov oC0, r0
```

SPIR-V:
```glsl
void main() {
    outColor = texture(tex0, inTexCoord);
}
```

---

## Test Cases

```cpp
TEST(ShaderTranslatorTest, SimpleVertexShader) {
    // Minimal MVP transform shader
    u32 microcode[] = { /* ... */ };
    
    ShaderTranslator translator;
    auto result = translator.translate({
        .type = XenosShaderInfo::Vertex,
        .microcode = {microcode, microcode + sizeof(microcode)/4}
    });
    
    EXPECT_FALSE(result.spirv.empty());
    EXPECT_EQ(result.spirv[0], 0x07230203); // SPIR-V magic
}

TEST(ShaderTranslatorTest, TextureSample) {
    // Simple texture fetch shader
    ShaderTranslator translator;
    auto result = translator.translate(/* pixel shader with tfetch */);
    
    // Verify SPIR-V contains OpImageSampleImplicitLod
    EXPECT_TRUE(contains_opcode(result.spirv, SpvOpImageSampleImplicitLod));
}
```

---

## Do NOT Touch

- Vulkan backend initialization (separate task)
- Command processor (separate task)
- eDRAM emulation (separate task)
- CPU code (`src/cpu/`)

---

## Success Criteria

1. ✅ Parse Xenos shader microcode format
2. ✅ Generate valid SPIR-V for simple vertex shaders
3. ✅ Generate valid SPIR-V for texture sampling
4. ✅ Shader cache prevents re-translation
5. ✅ At least 20 ALU operations supported

---

## Reference Materials

- Xenia's `gpu/shader_translator.cc` 
- Mesa3D Freedreno driver (similar microcode)
- SPIR-V spec: https://www.khronos.org/registry/SPIR-V/

---

*This task focuses only on shader translation. Vulkan backend and command processing are separate tasks.*

