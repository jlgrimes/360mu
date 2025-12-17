/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos Shader Translator Implementation
 * Converts Xbox 360 Xenos GPU shader microcode to SPIR-V for Vulkan
 * 
 * Reference: Xenia GPU shader documentation and reverse engineering
 */

#include "shader_translator.h"
#include "command_processor.h"
#include <cstring>
#include <fstream>
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-shader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[SHADER] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[SHADER ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// SPIR-V Constants (from spirv_builder.cpp, duplicated here for convenience)
//=============================================================================

namespace spv {
    // Execution model
    constexpr u32 ExecutionModelVertex = 0;
    constexpr u32 ExecutionModelFragment = 4;
    
    // Execution mode
    constexpr u32 ExecutionModeOriginUpperLeft = 7;
    
    // Storage class
    constexpr u32 StorageClassInput = 1;
    constexpr u32 StorageClassOutput = 3;
    constexpr u32 StorageClassUniform = 2;
    constexpr u32 StorageClassUniformConstant = 0;
    constexpr u32 StorageClassFunction = 7;
    constexpr u32 StorageClassPrivate = 6;
    
    // Decoration
    constexpr u32 DecorationLocation = 30;
    constexpr u32 DecorationBinding = 33;
    constexpr u32 DecorationDescriptorSet = 34;
    constexpr u32 DecorationBuiltIn = 11;
    constexpr u32 DecorationBlock = 2;
    constexpr u32 DecorationOffset = 35;
    
    // BuiltIn values
    constexpr u32 BuiltInPosition = 0;
    constexpr u32 BuiltInVertexIndex = 42;
    constexpr u32 BuiltInInstanceIndex = 43;
    constexpr u32 BuiltInFragCoord = 15;
    constexpr u32 BuiltInFrontFacing = 17;
    
    // GLSL.std.450 extended instructions
    constexpr u32 GLSLstd450Trunc = 3;
    constexpr u32 GLSLstd450FAbs = 4;
    constexpr u32 GLSLstd450Floor = 8;
    constexpr u32 GLSLstd450Fract = 10;
    constexpr u32 GLSLstd450Sqrt = 31;
    constexpr u32 GLSLstd450InverseSqrt = 32;
    constexpr u32 GLSLstd450Exp2 = 29;
    constexpr u32 GLSLstd450Log2 = 30;
    constexpr u32 GLSLstd450Sin = 13;
    constexpr u32 GLSLstd450Cos = 14;
    constexpr u32 GLSLstd450FMin = 37;
    constexpr u32 GLSLstd450FMax = 40;
    constexpr u32 GLSLstd450FClamp = 43;
    constexpr u32 GLSLstd450Length = 66;
    constexpr u32 GLSLstd450Normalize = 69;
    constexpr u32 GLSLstd450Cross = 68;
    constexpr u32 GLSLstd450Dot = 148;  // Custom - not in GLSL.std.450, use OpDot
}

//=============================================================================
// Xenos Shader Control Flow Opcodes
//=============================================================================

namespace xenos_cf {
    constexpr u8 NOP = 0;
    constexpr u8 EXEC = 1;
    constexpr u8 EXEC_END = 2;
    constexpr u8 COND_EXEC = 3;
    constexpr u8 COND_EXEC_END = 4;
    constexpr u8 COND_PRED_EXEC = 5;
    constexpr u8 COND_PRED_EXEC_END = 6;
    constexpr u8 LOOP_START = 7;
    constexpr u8 LOOP_END = 8;
    constexpr u8 COND_CALL = 9;
    constexpr u8 RETURN = 10;
    constexpr u8 COND_JMP = 11;
    constexpr u8 ALLOC = 12;
    constexpr u8 COND_EXEC_PRED_CLEAN = 13;
    constexpr u8 COND_EXEC_PRED_CLEAN_END = 14;
    constexpr u8 MARK_VS_FETCH_DONE = 15;
}

//=============================================================================
// ShaderMicrocode Implementation
//=============================================================================

Status ShaderMicrocode::parse(const void* data, u32 size, ShaderType type) {
    type_ = type;
    cf_instructions_.clear();
    alu_instructions_.clear();
    fetch_instructions_.clear();
    instructions_.clear();
    
    if (!data || size < 12) {
        LOGE("Invalid shader data");
        return Status::InvalidArgument;
    }
    
    // Copy raw instruction data
    u32 word_count = size / 4;
    instructions_.resize(word_count);
    memcpy(instructions_.data(), data, size);
    
    // Decode control flow instructions (first part of shader)
    decode_control_flow();
    
    return Status::Ok;
}

void ShaderMicrocode::decode_control_flow() {
    // Control flow instructions are 48 bits (packed as 2 per 96 bits)
    // They are stored at the beginning of the shader
    
    u32 cf_index = 0;
    bool end_found = false;
    
    while (cf_index < instructions_.size() && !end_found) {
        // Each 96-bit chunk contains 2 CF instructions
        u32 dword0 = instructions_[cf_index];
        u32 dword1 = cf_index + 1 < instructions_.size() ? instructions_[cf_index + 1] : 0;
        u32 dword2 = cf_index + 2 < instructions_.size() ? instructions_[cf_index + 2] : 0;
        
        // First CF instruction (lower 48 bits)
        ControlFlowInstruction cf0;
        cf0.word = dword0;
        cf0.opcode = (dword0 >> 23) & 0x1F;
        cf0.address = dword0 & 0x1FF;
        cf0.count = (dword0 >> 9) & 0x7;
        cf0.end_of_shader = (dword0 >> 22) & 1;
        cf0.predicated = (dword0 >> 21) & 1;
        cf0.condition = (dword0 >> 20) & 1;
        cf_instructions_.push_back(cf0);
        
        if (cf0.opcode == xenos_cf::EXEC || cf0.opcode == xenos_cf::EXEC_END) {
            // Decode ALU/Fetch clauses referenced by this CF instruction
            bool is_fetch = (dword0 >> 19) & 1;
            if (is_fetch) {
                decode_fetch_clause(cf0.address * 3, cf0.count + 1);
            } else {
                decode_alu_clause(cf0.address * 3, cf0.count + 1);
            }
        }
        
        if (cf0.end_of_shader || cf0.opcode == xenos_cf::EXEC_END) {
            end_found = true;
        }
        
        // Second CF instruction (upper 48 bits)
        if (!end_found) {
            ControlFlowInstruction cf1;
            cf1.word = dword1;
            u64 upper = (static_cast<u64>(dword2) << 32) | static_cast<u64>(dword1);
            cf1.opcode = (upper >> 23) & 0x1F;
            cf1.address = upper & 0x1FF;
            cf1.count = (upper >> 9) & 0x7;
            cf1.end_of_shader = (upper >> 22) & 1;
            cf1.predicated = (upper >> 21) & 1;
            cf1.condition = (upper >> 20) & 1;
            cf_instructions_.push_back(cf1);
            
            if (cf1.opcode == xenos_cf::EXEC || cf1.opcode == xenos_cf::EXEC_END) {
                bool is_fetch = (upper >> 19) & 1;
                if (is_fetch) {
                    decode_fetch_clause(cf1.address * 3, cf1.count + 1);
                } else {
                    decode_alu_clause(cf1.address * 3, cf1.count + 1);
                }
            }
            
            if (cf1.end_of_shader || cf1.opcode == xenos_cf::EXEC_END) {
                end_found = true;
            }
        }
        
        cf_index += 3;  // Move to next 96-bit chunk
    }
}

void ShaderMicrocode::decode_alu_clause(u32 address, u32 count) {
    // ALU instructions are 96 bits (3 dwords)
    for (u32 i = 0; i < count && (address + 2) < instructions_.size(); i++) {
        AluInstruction inst;
        inst.words[0] = instructions_[address];
        inst.words[1] = instructions_[address + 1];
        inst.words[2] = instructions_[address + 2];
        
        // Decode scalar operation (bits 54-59 of the 96-bit instruction)
        inst.scalar_opcode = (inst.words[1] >> 22) & 0x3F;
        
        // Decode vector operation (bits 48-53)
        inst.vector_opcode = (inst.words[1] >> 16) & 0x3F;
        
        // Decode destination register (bits 32-38)
        inst.dest_reg = inst.words[1] & 0x7F;
        
        // Decode source registers (packed in words[0] and words[2])
        inst.src_regs[0] = inst.words[0] & 0x7F;
        inst.src_regs[1] = (inst.words[0] >> 7) & 0x7F;
        inst.src_regs[2] = (inst.words[0] >> 14) & 0x7F;
        
        // Source modifiers
        inst.abs[0] = (inst.words[0] >> 21) & 1;
        inst.abs[1] = (inst.words[0] >> 22) & 1;
        inst.abs[2] = (inst.words[0] >> 23) & 1;
        inst.negate[0] = (inst.words[0] >> 24) & 1;
        inst.negate[1] = (inst.words[0] >> 25) & 1;
        inst.negate[2] = (inst.words[0] >> 26) & 1;
        
        // Write mask (bits 39-42 of word 1)
        inst.write_mask = (inst.words[1] >> 7) & 0xF;
        
        // Export data flag
        inst.export_data = (inst.words[1] >> 15) & 1;
        inst.export_type = (inst.words[1] >> 11) & 0xF;
        
        alu_instructions_.push_back(inst);
        address += 3;
    }
}

void ShaderMicrocode::decode_fetch_clause(u32 address, u32 count) {
    // Fetch instructions are also 96 bits
    for (u32 i = 0; i < count && (address + 2) < instructions_.size(); i++) {
        FetchInstruction inst;
        inst.words[0] = instructions_[address];
        inst.words[1] = instructions_[address + 1];
        inst.words[2] = instructions_[address + 2];
        
        // Decode opcode (bits 0-4)
        inst.opcode = inst.words[0] & 0x1F;
        
        // Destination register (bits 5-11)
        inst.dest_reg = (inst.words[0] >> 5) & 0x7F;
        
        // Source register (bits 12-18)
        inst.src_reg = (inst.words[0] >> 12) & 0x7F;
        
        // Constant index (bits 19-24)
        inst.const_index = (inst.words[0] >> 19) & 0x3F;
        
        // Fetch type (0 = vertex, 1 = texture)
        inst.fetch_type = (inst.words[0] >> 25) & 1;
        
        // Offset (bits 26-31 + bits 0-7 of word 1)
        inst.offset = ((inst.words[0] >> 26) & 0x3F) | ((inst.words[1] & 0xFF) << 6);
        
        // Data format (bits 8-15 of word 1)
        inst.data_format = (inst.words[1] >> 8) & 0x3F;
        
        // Signed
        inst.signed_rf = (inst.words[1] >> 14) & 1;
        
        // Num format (bits 15-16 of word 1)
        inst.num_format = (inst.words[1] >> 15) & 3;
        
        // Stride (bits 20-31 of word 1)
        inst.stride = (inst.words[1] >> 20) & 0xFFF;
        
        fetch_instructions_.push_back(inst);
        address += 3;
    }
}

//=============================================================================
// ShaderTranslator Implementation
//=============================================================================

ShaderTranslator::ShaderTranslator() = default;

ShaderTranslator::~ShaderTranslator() {
    shutdown();
}

Status ShaderTranslator::initialize(const std::string& cache_path) {
    cache_path_ = cache_path;
    
    // Try to load cached shaders
    if (!cache_path.empty()) {
        load_cache();
    }
    
    LOGI("Shader translator initialized");
    return Status::Ok;
}

void ShaderTranslator::shutdown() {
    if (!cache_path_.empty()) {
        save_cache();
    }
    cache_.clear();
}

u64 ShaderTranslator::compute_hash(const void* data, u32 size) {
    // FNV-1a hash
    u64 hash = 14695981039346656037ULL;
    const u8* bytes = static_cast<const u8*>(data);
    for (u32 i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

const std::vector<u32>* ShaderTranslator::get_cached(u64 hash) const {
    auto it = cache_.find(hash);
    return it != cache_.end() ? &it->second : nullptr;
}

void ShaderTranslator::cache(u64 hash, std::vector<u32> spirv) {
    cache_[hash] = std::move(spirv);
}

void ShaderTranslator::save_cache() {
    if (cache_path_.empty() || cache_.empty()) return;
    
    std::string path = cache_path_ + "/shader_cache.bin";
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    
    // Write number of entries
    u32 count = static_cast<u32>(cache_.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    
    // Write each entry
    for (const auto& [hash, spirv] : cache_) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        u32 size = static_cast<u32>(spirv.size());
        file.write(reinterpret_cast<const char*>(&size), sizeof(size));
        file.write(reinterpret_cast<const char*>(spirv.data()), size * sizeof(u32));
    }
    
    LOGI("Saved %u cached shaders", count);
}

void ShaderTranslator::load_cache() {
    if (cache_path_.empty()) return;
    
    std::string path = cache_path_ + "/shader_cache.bin";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    
    u32 count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    
    for (u32 i = 0; i < count && file; i++) {
        u64 hash;
        file.read(reinterpret_cast<char*>(&hash), sizeof(hash));
        
        u32 size;
        file.read(reinterpret_cast<char*>(&size), sizeof(size));
        
        std::vector<u32> spirv(size);
        file.read(reinterpret_cast<char*>(spirv.data()), size * sizeof(u32));
        
        cache_[hash] = std::move(spirv);
    }
    
    LOGI("Loaded %u cached shaders", static_cast<u32>(cache_.size()));
}

std::vector<u32> ShaderTranslator::translate(const void* microcode, u32 size, ShaderType type) {
    // Check cache first
    u64 hash = compute_hash(microcode, size);
    const auto* cached = get_cached(hash);
    if (cached) {
        stats_.cache_hits++;
        return *cached;
    }
    stats_.cache_misses++;
    
    // Parse the microcode
    ShaderMicrocode parsed;
    if (parsed.parse(microcode, size, type) != Status::Ok) {
        LOGE("Failed to parse shader microcode");
        return {};
    }
    
    // Analyze shader to determine what resources it uses
    ShaderInfo info = analyze(microcode, size, type);
    
    // Create translation context
    TranslationContext ctx;
    ctx.type = type;
    ctx.microcode = &parsed;
    ctx.info = info;
    
    // Begin SPIR-V generation
    ctx.builder.begin(type);
    
    // Import GLSL.std.450 extension
    ctx.glsl_ext = ctx.builder.import_extension("GLSL.std.450");
    
    // Setup types
    setup_types(ctx);
    
    // Setup cached constants
    setup_constants(ctx);
    
    // Setup inputs/outputs based on shader type
    setup_inputs(ctx);
    setup_outputs(ctx);
    setup_uniforms(ctx);
    
    // Setup samplers if needed
    setup_samplers(ctx, info);
    
    // Setup interpolants
    setup_interpolants(ctx, info);
    
    // Create main function
    u32 void_func_type = ctx.builder.type_function(ctx.void_type, {});
    ctx.builder.function_begin(ctx.void_type, void_func_type);
    ctx.main_function = ctx.builder.allocate_id() - 1;  // Function ID
    
    u32 entry_label = ctx.builder.allocate_id();
    ctx.builder.label(entry_label);
    
    // Initialize temporary registers and other variables
    setup_temporaries(ctx);
    
    // Initialize temps to zero
    for (u32 i = 0; i < 128; i++) {
        ctx.builder.store(ctx.temp_vars[i], ctx.const_vec4_zero);
    }
    
    // Initialize predicate to false
    u32 false_val = ctx.builder.const_bool(false);
    ctx.builder.store(ctx.predicate_var, false_val);
    
    // Initialize address register to 0
    u32 zero_int = ctx.builder.const_int(0);
    ctx.builder.store(ctx.address_reg_var, zero_int);
    
    // Translate shader body
    translate_control_flow(ctx, parsed);
    
    // Write outputs if not already done by exports
    if (type == ShaderType::Vertex) {
        // Export position from register 0 if no explicit position export was found
        if (!info.exports_position) {
            u32 pos = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[0]);
            ctx.builder.store(ctx.position_var, pos);
        }
    } else {
        // Export fragment color from register 0 if no explicit color export was found
        if (info.color_export_count == 0) {
            u32 color = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[0]);
            ctx.builder.store(ctx.frag_color_var, color);
        }
    }
    
    ctx.builder.return_void();
    ctx.builder.function_end();
    
    // Collect interface variables for entry point
    std::vector<u32> interface_vars;
    if (type == ShaderType::Vertex) {
        interface_vars.push_back(ctx.position_var);
        if (ctx.vertex_id_var != 0) interface_vars.push_back(ctx.vertex_id_var);
        if (ctx.instance_id_var != 0) interface_vars.push_back(ctx.instance_id_var);
        if (ctx.point_size_var != 0) interface_vars.push_back(ctx.point_size_var);
        
        // Add interpolant outputs
        for (u32 i = 0; i < 16; i++) {
            if (ctx.interpolant_vars[i] != 0) {
                interface_vars.push_back(ctx.interpolant_vars[i]);
            }
        }
    } else {
        if (ctx.frag_coord_var != 0) interface_vars.push_back(ctx.frag_coord_var);
        if (ctx.front_facing_var != 0) interface_vars.push_back(ctx.front_facing_var);
        if (ctx.frag_color_var != 0) interface_vars.push_back(ctx.frag_color_var);
        if (ctx.frag_depth_var != 0) interface_vars.push_back(ctx.frag_depth_var);
        
        // Add color outputs
        for (u32 i = 0; i < 4; i++) {
            if (ctx.color_outputs[i] != 0) {
                interface_vars.push_back(ctx.color_outputs[i]);
            }
        }
        
        // Add interpolant inputs
        for (u32 i = 0; i < 16; i++) {
            if (ctx.interpolant_vars[i] != 0) {
                interface_vars.push_back(ctx.interpolant_vars[i]);
            }
        }
    }
    
    u32 entry_func = ctx.main_function;
    ctx.builder.entry_point(
        type == ShaderType::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment,
        entry_func, "main", interface_vars);
    
    if (type == ShaderType::Pixel) {
        ctx.builder.execution_mode(entry_func, spv::ExecutionModeOriginUpperLeft);
    }
    
    // Generate final SPIR-V
    std::vector<u32> spirv = ctx.builder.end();
    
    // Update statistics
    stats_.shaders_translated++;
    stats_.total_spirv_size += spirv.size() * sizeof(u32);
    stats_.total_microcode_size += size;
    
    // Cache the result
    cache(hash, spirv);
    
    LOGI("Translated %s shader: %u bytes microcode -> %zu words SPIR-V (temps=%u, consts=%u)",
         type == ShaderType::Vertex ? "vertex" : "pixel",
         size, spirv.size(), info.temp_register_count, info.max_const_register);
    
    return spirv;
}

void ShaderTranslator::setup_types(TranslationContext& ctx) {
    ctx.void_type = ctx.builder.type_void();
    ctx.bool_type = ctx.builder.type_bool();
    ctx.int_type = ctx.builder.type_int(32, true);
    ctx.uint_type = ctx.builder.type_int(32, false);
    ctx.float_type = ctx.builder.type_float(32);
    ctx.vec2_type = ctx.builder.type_vector(ctx.float_type, 2);
    ctx.vec3_type = ctx.builder.type_vector(ctx.float_type, 3);
    ctx.vec4_type = ctx.builder.type_vector(ctx.float_type, 4);
    ctx.ivec2_type = ctx.builder.type_vector(ctx.int_type, 2);
    ctx.ivec3_type = ctx.builder.type_vector(ctx.int_type, 3);
    ctx.ivec4_type = ctx.builder.type_vector(ctx.int_type, 4);
    ctx.uvec4_type = ctx.builder.type_vector(ctx.uint_type, 4);
    ctx.bvec4_type = ctx.builder.type_vector(ctx.bool_type, 4);
    ctx.mat4_type = ctx.builder.type_matrix(ctx.vec4_type, 4);
    
    // Pointer types (cached for reuse)
    ctx.float_func_ptr = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.float_type);
    ctx.vec4_func_ptr = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.vec4_type);
    ctx.vec4_uniform_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, ctx.vec4_type);
    ctx.bool_func_ptr = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.bool_type);
    ctx.int_func_ptr = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.int_type);
}

void ShaderTranslator::setup_inputs(TranslationContext& ctx) {
    if (ctx.type == ShaderType::Vertex) {
        // gl_VertexIndex
        u32 uint_ptr = ctx.builder.type_pointer(spv::StorageClassInput, ctx.uint_type);
        ctx.vertex_id_var = ctx.builder.variable(uint_ptr, spv::StorageClassInput);
        ctx.builder.decorate(ctx.vertex_id_var, spv::DecorationBuiltIn, {spv::BuiltInVertexIndex});
    } else {
        // gl_FragCoord
        u32 vec4_in_ptr = ctx.builder.type_pointer(spv::StorageClassInput, ctx.vec4_type);
        ctx.frag_coord_var = ctx.builder.variable(vec4_in_ptr, spv::StorageClassInput);
        ctx.builder.decorate(ctx.frag_coord_var, spv::DecorationBuiltIn, {spv::BuiltInFragCoord});
        
        // gl_FrontFacing
        u32 bool_in_ptr = ctx.builder.type_pointer(spv::StorageClassInput, ctx.bool_type);
        ctx.front_facing_var = ctx.builder.variable(bool_in_ptr, spv::StorageClassInput);
        ctx.builder.decorate(ctx.front_facing_var, spv::DecorationBuiltIn, {spv::BuiltInFrontFacing});
    }
}

void ShaderTranslator::setup_outputs(TranslationContext& ctx) {
    if (ctx.type == ShaderType::Vertex) {
        // gl_Position
        u32 vec4_out_ptr = ctx.builder.type_pointer(spv::StorageClassOutput, ctx.vec4_type);
        ctx.position_var = ctx.builder.variable(vec4_out_ptr, spv::StorageClassOutput);
        ctx.builder.decorate(ctx.position_var, spv::DecorationBuiltIn, {spv::BuiltInPosition});
    } else {
        // Fragment color output
        u32 vec4_out_ptr = ctx.builder.type_pointer(spv::StorageClassOutput, ctx.vec4_type);
        ctx.frag_color_var = ctx.builder.variable(vec4_out_ptr, spv::StorageClassOutput);
        ctx.builder.decorate(ctx.frag_color_var, spv::DecorationLocation, {0});
    }
}

void ShaderTranslator::setup_uniforms(TranslationContext& ctx) {
    // Create uniform buffer for constants (256 vec4 constants)
    u32 const_array_type = ctx.builder.type_array(ctx.vec4_type, 
        ctx.builder.const_uint(256));
    
    u32 uniform_struct = ctx.builder.type_struct({const_array_type});
    ctx.builder.decorate(uniform_struct, spv::DecorationBlock);
    ctx.builder.member_decorate(uniform_struct, 0, spv::DecorationOffset, {0});
    
    u32 uniform_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, uniform_struct);
    
    if (ctx.type == ShaderType::Vertex) {
        ctx.vertex_constants_var = ctx.builder.variable(uniform_ptr, spv::StorageClassUniform);
        ctx.builder.decorate(ctx.vertex_constants_var, spv::DecorationDescriptorSet, {0});
        ctx.builder.decorate(ctx.vertex_constants_var, spv::DecorationBinding, {0});
    } else {
        ctx.pixel_constants_var = ctx.builder.variable(uniform_ptr, spv::StorageClassUniform);
        ctx.builder.decorate(ctx.pixel_constants_var, spv::DecorationDescriptorSet, {0});
        ctx.builder.decorate(ctx.pixel_constants_var, spv::DecorationBinding, {1});
    }
}

void ShaderTranslator::translate_control_flow(TranslationContext& ctx, const ShaderMicrocode& microcode) {
    // Process each control flow instruction
    for (const auto& cf : microcode.cf_instructions()) {
        switch (cf.opcode) {
            case xenos_cf::NOP:
                // No operation
                break;
                
            case xenos_cf::EXEC:
            case xenos_cf::EXEC_END:
                // Execute ALU or fetch clause
                // The clause has already been decoded, translate it
                break;
                
            case xenos_cf::COND_EXEC:
            case xenos_cf::COND_EXEC_END:
                // Conditional execution based on predicate
                // TODO: Implement predicated execution
                break;
                
            case xenos_cf::LOOP_START:
                // TODO: Implement loop support
                break;
                
            case xenos_cf::LOOP_END:
                // TODO: Implement loop support  
                break;
                
            case xenos_cf::ALLOC:
                // Allocation - handled at pipeline setup
                break;
                
            default:
                LOGD("Unhandled CF opcode: %d", cf.opcode);
                break;
        }
    }
    
    // Translate ALU instructions
    for (const auto& inst : microcode.alu_instructions()) {
        translate_alu_instruction(ctx, inst);
    }
    
    // Translate fetch instructions
    for (const auto& inst : microcode.fetch_instructions()) {
        translate_fetch_instruction(ctx, inst);
    }
}

void ShaderTranslator::translate_alu_instruction(TranslationContext& ctx, 
                                                  const ShaderMicrocode::AluInstruction& inst) {
    // Check predication
    if (inst.predicated) {
        u32 pred = ctx.builder.load(ctx.bool_type, ctx.predicate_var);
        if (!inst.predicate_condition) {
            pred = ctx.builder.logical_not(ctx.bool_type, pred);
        }
        // For simplicity, we don't emit conditional blocks for every instruction
        // A more complete impl would create selection blocks
    }
    
    // Get source values with swizzle and modifiers
    std::vector<u32> sources;
    for (int i = 0; i < 3; i++) {
        u32 src = get_source_with_swizzle(ctx, inst.src_regs[i], inst.src_swizzles[i], 
                                          inst.src_abs[i], inst.src_negate[i], inst.src_is_const[i]);
        sources.push_back(src);
    }
    
    // Execute vector operation
    u32 vector_result = 0;
    if (inst.vector_opcode != 0 || inst.vector_write_mask != 0) {
        vector_result = translate_vector_op(ctx, static_cast<AluVectorOp>(inst.vector_opcode), 
                                           sources, ctx.vec4_type);
        
        // Apply saturation if requested
        if (inst.vector_saturate && vector_result != 0) {
            vector_result = apply_saturate(ctx, vector_result);
        }
        
        // Store vector result for next instruction
        ctx.prev_vector = vector_result;
    }
    
    // Execute scalar operation  
    u32 scalar_result = 0;
    if (inst.scalar_opcode != 0) {
        // Scalar ops use the w component of source 2 (src[2].w)
        u32 scalar_src = ctx.builder.composite_extract(ctx.float_type, sources[2], {3});
        scalar_result = translate_scalar_op(ctx, static_cast<AluScalarOp>(inst.scalar_opcode),
                                           scalar_src, ctx.float_type);
        
        // Apply saturation if requested
        if (inst.scalar_saturate && scalar_result != 0) {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, 
                                                 spv::GLSLstd450FClamp, {scalar_result, zero, one});
        }
        
        // Store scalar result for next instruction
        ctx.prev_scalar = scalar_result;
    }
    
    // Combine results and write to destination
    u32 final_result = vector_result;
    u8 final_write_mask = inst.vector_write_mask;
    
    if (scalar_result != 0 && vector_result != 0) {
        // Both scalar and vector results - combine them
        // Scalar typically writes to W, vector writes to XYZ
        if (inst.scalar_write_mask & 0x8) {  // W component
            final_result = ctx.builder.composite_insert(ctx.vec4_type, scalar_result, vector_result, {3});
            final_write_mask |= inst.scalar_write_mask;
        }
    } else if (scalar_result != 0) {
        // Only scalar result - broadcast to requested components
        final_result = ctx.builder.composite_construct(ctx.vec4_type, 
            {scalar_result, scalar_result, scalar_result, scalar_result});
        final_write_mask = inst.scalar_write_mask;
    }
    
    // Handle export
    if (inst.export_data && final_result != 0) {
        handle_export(ctx, final_result, inst.export_type, inst.export_index);
    }
    
    // Write to destination register
    if (final_result != 0 && final_write_mask != 0) {
        u8 dest_reg = inst.vector_opcode != 0 ? inst.vector_dest : inst.scalar_dest;
        if (inst.export_data) {
            // Export instructions may not write to registers
        } else {
            write_dest_register(ctx, dest_reg, final_result, final_write_mask);
        }
    }
}

void ShaderTranslator::translate_fetch_instruction(TranslationContext& ctx,
                                                    const ShaderMicrocode::FetchInstruction& inst) {
    u32 result;
    
    if (inst.fetch_type == 0) {
        // Vertex fetch
        result = translate_vertex_fetch(ctx, inst);
    } else {
        // Texture fetch
        result = translate_texture_fetch(ctx, inst);
    }
    
    // Apply destination swizzle if present
    if (inst.dest_swizzle != 0xE4) {
        result = apply_swizzle(ctx, result, inst.dest_swizzle);
    }
    
    // Store result to destination register
    ctx.builder.store(ctx.temp_vars[inst.dest_reg & 0x7F], result);
}

u32 ShaderTranslator::translate_scalar_op(TranslationContext& ctx, AluScalarOp op, 
                                          u32 src, u32 type) {
    switch (op) {
        case AluScalarOp::ADDs:
            return src;  // Single operand add (identity)
            
        case AluScalarOp::MULs:
            return ctx.builder.f_mul(type, src, src);
            
        case AluScalarOp::MAXs:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FMax, {src, src});
            
        case AluScalarOp::MINs:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FMin, {src, src});
            
        case AluScalarOp::FRACs:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Fract, {src});
            
        case AluScalarOp::TRUNCs:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Trunc, {src});
            
        case AluScalarOp::FLOORs:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Floor, {src});
            
        case AluScalarOp::EXP_IEEE:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Exp2, {src});
            
        case AluScalarOp::LOG_IEEE:
        case AluScalarOp::LOG_CLAMP:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Log2, {src});
            
        case AluScalarOp::RECIP_IEEE:
        case AluScalarOp::RECIP_CLAMP:
        case AluScalarOp::RECIP_FF: {
            u32 one = ctx.builder.const_float(1.0f);
            return ctx.builder.f_div(type, one, src);
        }
            
        case AluScalarOp::RECIPSQ_IEEE:
        case AluScalarOp::RECIPSQ_CLAMP:
        case AluScalarOp::RECIPSQ_FF:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450InverseSqrt, {src});
            
        case AluScalarOp::SQRT_IEEE:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Sqrt, {src});
            
        case AluScalarOp::SIN:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Sin, {src});
            
        case AluScalarOp::COS:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Cos, {src});
            
        case AluScalarOp::SETEs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, src, zero);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::SETGTs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, src, zero);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::SETGTEs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            u32 cmp = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, src, zero);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::SETNEs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, src, zero);
            return ctx.builder.select(type, cmp, one, zero);
        }
        
        case AluScalarOp::ADD_PREVs:
            // Add with previous scalar result
            if (ctx.prev_scalar != 0) {
                return ctx.builder.f_add(type, src, ctx.prev_scalar);
            }
            return src;
            
        case AluScalarOp::MUL_PREVs:
            // Multiply with previous scalar result
            if (ctx.prev_scalar != 0) {
                return ctx.builder.f_mul(type, src, ctx.prev_scalar);
            }
            return src;
            
        case AluScalarOp::MUL_PREV2s:
            // Multiply previous two results
            if (ctx.prev_scalar != 0 && ctx.prev_vector != 0) {
                u32 prev_w = ctx.builder.composite_extract(ctx.float_type, ctx.prev_vector, {3});
                return ctx.builder.f_mul(type, ctx.prev_scalar, prev_w);
            }
            return src;
            
        case AluScalarOp::SUBs: {
            // Subtract (src - src, effectively 0 for single operand)
            u32 zero = ctx.builder.const_float(0.0f);
            return zero;
        }
            
        case AluScalarOp::SUB_PREVs:
            // Subtract previous
            if (ctx.prev_scalar != 0) {
                return ctx.builder.f_sub(type, src, ctx.prev_scalar);
            }
            return src;
            
        case AluScalarOp::MOVAs: {
            // Move to address register (convert to int)
            u32 int_val = ctx.builder.convert_f_to_s(ctx.int_type, src);
            ctx.builder.store(ctx.address_reg_var, int_val);
            return src;
        }
            
        case AluScalarOp::MOVA_FLOORs: {
            // Move floor to address register
            u32 floored = ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Floor, {src});
            u32 int_val = ctx.builder.convert_f_to_s(ctx.int_type, floored);
            ctx.builder.store(ctx.address_reg_var, int_val);
            return floored;
        }
            
        case AluScalarOp::PRED_SETEs: {
            // Predicate set if equal to zero
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, src, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            u32 one = ctx.builder.const_float(1.0f);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::PRED_SETNEs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, src, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            u32 one = ctx.builder.const_float(1.0f);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::PRED_SETGTs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, src, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            u32 one = ctx.builder.const_float(1.0f);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::PRED_SETGTEs: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, src, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            u32 one = ctx.builder.const_float(1.0f);
            return ctx.builder.select(type, cmp, one, zero);
        }
            
        case AluScalarOp::PRED_SET_INVs: {
            // Invert predicate: result = 1.0 - src (clamped)
            u32 one = ctx.builder.const_float(1.0f);
            u32 result = ctx.builder.f_sub(type, one, src);
            u32 zero = ctx.builder.const_float(0.0f);
            result = ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FClamp, {result, zero, one});
            u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, result, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            return result;
        }
            
        case AluScalarOp::PRED_SET_POPs: {
            // Pop predicate stack (simplified - just return src)
            u32 zero = ctx.builder.const_float(0.0f);
            u32 result = ctx.builder.f_sub(type, src, ctx.builder.const_float(1.0f));
            result = ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FMax, {result, zero});
            u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, result, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            return result;
        }
            
        case AluScalarOp::PRED_SET_CLRs: {
            // Clear predicate
            u32 false_val = ctx.builder.const_bool(false);
            ctx.builder.store(ctx.predicate_var, false_val);
            return ctx.builder.const_float(0.0f);
        }
            
        case AluScalarOp::PRED_SET_RESTOREs: {
            // Restore predicate from src
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, src, zero);
            ctx.builder.store(ctx.predicate_var, cmp);
            return src;
        }
            
        case AluScalarOp::KILLEs: {
            // Kill if equal to zero
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, src, zero);
            emit_kill(ctx, cmp);
            return src;
        }
            
        case AluScalarOp::KILLGTs: {
            // Kill if greater than zero
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, src, zero);
            emit_kill(ctx, cmp);
            return src;
        }
            
        case AluScalarOp::KILLGTEs: {
            // Kill if greater than or equal to zero
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, src, zero);
            emit_kill(ctx, cmp);
            return src;
        }
            
        case AluScalarOp::KILLNEs: {
            // Kill if not equal to zero
            u32 zero = ctx.builder.const_float(0.0f);
            u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, src, zero);
            emit_kill(ctx, cmp);
            return src;
        }
            
        case AluScalarOp::KILLONEs: {
            // Kill if equal to one
            u32 one = ctx.builder.const_float(1.0f);
            u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, src, one);
            emit_kill(ctx, cmp);
            return src;
        }
            
        case AluScalarOp::MUL_CONST_0:
        case AluScalarOp::MUL_CONST_1: {
            // Multiply by constant (index determined by opcode)
            u32 const_idx = (op == AluScalarOp::MUL_CONST_0) ? 0 : 1;
            u32 const_val = get_constant(ctx, const_idx);
            u32 const_x = ctx.builder.composite_extract(ctx.float_type, const_val, {0});
            return ctx.builder.f_mul(type, src, const_x);
        }
            
        case AluScalarOp::ADD_CONST_0:
        case AluScalarOp::ADD_CONST_1: {
            u32 const_idx = (op == AluScalarOp::ADD_CONST_0) ? 0 : 1;
            u32 const_val = get_constant(ctx, const_idx);
            u32 const_x = ctx.builder.composite_extract(ctx.float_type, const_val, {0});
            return ctx.builder.f_add(type, src, const_x);
        }
            
        case AluScalarOp::SUB_CONST_0:
        case AluScalarOp::SUB_CONST_1: {
            u32 const_idx = (op == AluScalarOp::SUB_CONST_0) ? 0 : 1;
            u32 const_val = get_constant(ctx, const_idx);
            u32 const_x = ctx.builder.composite_extract(ctx.float_type, const_val, {0});
            return ctx.builder.f_sub(type, src, const_x);
        }
            
        case AluScalarOp::RETAIN_PREV:
            // Return previous scalar result
            return ctx.prev_scalar != 0 ? ctx.prev_scalar : src;
            
        default:
            // Unknown op - return source unchanged
            return src;
    }
}

u32 ShaderTranslator::translate_vector_op(TranslationContext& ctx, AluVectorOp op,
                                          const std::vector<u32>& sources, u32 type) {
    if (sources.size() < 2) return 0;
    
    u32 src0 = sources[0];
    u32 src1 = sources.size() > 1 ? sources[1] : src0;
    u32 src2 = sources.size() > 2 ? sources[2] : src0;
    
    switch (op) {
        case AluVectorOp::ADDv:
            return ctx.builder.f_add(type, src0, src1);
            
        case AluVectorOp::MULv:
            return ctx.builder.f_mul(type, src0, src1);
            
        case AluVectorOp::MAXv:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FMax, {src0, src1});
            
        case AluVectorOp::MINv:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450FMin, {src0, src1});
            
        case AluVectorOp::FRACv:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Fract, {src0});
            
        case AluVectorOp::TRUNCv:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Trunc, {src0});
            
        case AluVectorOp::FLOORv:
            return ctx.builder.ext_inst(type, ctx.glsl_ext, spv::GLSLstd450Floor, {src0});
            
        case AluVectorOp::MULADDv: {
            // src0 * src1 + src2
            u32 mul = ctx.builder.f_mul(type, src0, src1);
            return ctx.builder.f_add(type, mul, src2);
        }
            
        case AluVectorOp::DOT4v: {
            // 4-component dot product, broadcast to all components
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 y0 = ctx.builder.composite_extract(ctx.float_type, src0, {1});
            u32 z0 = ctx.builder.composite_extract(ctx.float_type, src0, {2});
            u32 w0 = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            u32 x1 = ctx.builder.composite_extract(ctx.float_type, src1, {0});
            u32 y1 = ctx.builder.composite_extract(ctx.float_type, src1, {1});
            u32 z1 = ctx.builder.composite_extract(ctx.float_type, src1, {2});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            
            u32 px = ctx.builder.f_mul(ctx.float_type, x0, x1);
            u32 py = ctx.builder.f_mul(ctx.float_type, y0, y1);
            u32 pz = ctx.builder.f_mul(ctx.float_type, z0, z1);
            u32 pw = ctx.builder.f_mul(ctx.float_type, w0, w1);
            
            u32 sum = ctx.builder.f_add(ctx.float_type, px, py);
            sum = ctx.builder.f_add(ctx.float_type, sum, pz);
            sum = ctx.builder.f_add(ctx.float_type, sum, pw);
            
            return ctx.builder.composite_construct(type, {sum, sum, sum, sum});
        }
            
        case AluVectorOp::DOT3v: {
            // 3-component dot product
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 y0 = ctx.builder.composite_extract(ctx.float_type, src0, {1});
            u32 z0 = ctx.builder.composite_extract(ctx.float_type, src0, {2});
            u32 x1 = ctx.builder.composite_extract(ctx.float_type, src1, {0});
            u32 y1 = ctx.builder.composite_extract(ctx.float_type, src1, {1});
            u32 z1 = ctx.builder.composite_extract(ctx.float_type, src1, {2});
            
            u32 px = ctx.builder.f_mul(ctx.float_type, x0, x1);
            u32 py = ctx.builder.f_mul(ctx.float_type, y0, y1);
            u32 pz = ctx.builder.f_mul(ctx.float_type, z0, z1);
            
            u32 sum = ctx.builder.f_add(ctx.float_type, px, py);
            sum = ctx.builder.f_add(ctx.float_type, sum, pz);
            
            u32 w = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            return ctx.builder.composite_construct(type, {sum, sum, sum, w});
        }
            
        case AluVectorOp::SETEv: {
            // Set if equal (per component)
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            u32 zero_vec = ctx.builder.composite_construct(type, {zero, zero, zero, zero});
            u32 one_vec = ctx.builder.composite_construct(type, {one, one, one, one});
            
            // Compare each component
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, a, b);
                results.push_back(ctx.builder.select(ctx.float_type, cmp, one, zero));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::SETGTv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, a, b);
                results.push_back(ctx.builder.select(ctx.float_type, cmp, one, zero));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::CNDEv: {
            // Conditional select: src0 == 0 ? src1 : src2
            u32 zero = ctx.builder.const_float(0.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 cond = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 a = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src2, {static_cast<u32>(i)});
                u32 is_zero = ctx.builder.f_ord_equal(ctx.bool_type, cond, zero);
                results.push_back(ctx.builder.select(ctx.float_type, is_zero, a, b));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::CNDGTv: {
            // Conditional select: src0 > 0 ? src1 : src2
            u32 zero = ctx.builder.const_float(0.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 cond = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 a = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src2, {static_cast<u32>(i)});
                u32 is_gt = ctx.builder.f_ord_greater_than(ctx.bool_type, cond, zero);
                results.push_back(ctx.builder.select(ctx.float_type, is_gt, a, b));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::SETGTEv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, a, b);
                results.push_back(ctx.builder.select(ctx.float_type, cmp, one, zero));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::SETNEv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, a, b);
                results.push_back(ctx.builder.select(ctx.float_type, cmp, one, zero));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::CNDGTEv: {
            // Conditional select: src0 >= 0 ? src1 : src2
            u32 zero = ctx.builder.const_float(0.0f);
            
            std::vector<u32> results;
            for (int i = 0; i < 4; i++) {
                u32 cond = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 a = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src2, {static_cast<u32>(i)});
                u32 is_gte = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, cond, zero);
                results.push_back(ctx.builder.select(ctx.float_type, is_gte, a, b));
            }
            return ctx.builder.composite_construct(type, results);
        }
            
        case AluVectorOp::DOT2ADDv: {
            // 2-component dot product + add (src2.x)
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 y0 = ctx.builder.composite_extract(ctx.float_type, src0, {1});
            u32 x1 = ctx.builder.composite_extract(ctx.float_type, src1, {0});
            u32 y1 = ctx.builder.composite_extract(ctx.float_type, src1, {1});
            u32 add = ctx.builder.composite_extract(ctx.float_type, src2, {0});
            
            u32 px = ctx.builder.f_mul(ctx.float_type, x0, x1);
            u32 py = ctx.builder.f_mul(ctx.float_type, y0, y1);
            u32 dot = ctx.builder.f_add(ctx.float_type, px, py);
            u32 result = ctx.builder.f_add(ctx.float_type, dot, add);
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::CUBEv: {
            // Cube map coordinate generation
            return emit_cube_vector(ctx, src0, src1);
        }
            
        case AluVectorOp::MAX4v: {
            // Maximum of all 4 components
            u32 x = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 y = ctx.builder.composite_extract(ctx.float_type, src0, {1});
            u32 z = ctx.builder.composite_extract(ctx.float_type, src0, {2});
            u32 w = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            
            u32 max_xy = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FMax, {x, y});
            u32 max_zw = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FMax, {z, w});
            u32 result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FMax, {max_xy, max_zw});
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::PRED_SETE_PUSHv: {
            // Predicate set if equal, push to stack
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            // Compare src0.w == 0 && src1.w == 0
            u32 w0 = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            u32 cmp0 = ctx.builder.f_ord_equal(ctx.bool_type, w0, zero);
            u32 cmp1 = ctx.builder.f_ord_equal(ctx.bool_type, w1, zero);
            u32 cmp = ctx.builder.logical_and(ctx.bool_type, cmp0, cmp1);
            ctx.builder.store(ctx.predicate_var, cmp);
            
            // Result: src0.x == 0 ? 0 : src0.x + 1
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 is_zero = ctx.builder.f_ord_equal(ctx.bool_type, x0, zero);
            u32 inc = ctx.builder.f_add(ctx.float_type, x0, one);
            u32 result = ctx.builder.select(ctx.float_type, is_zero, zero, inc);
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::PRED_SETNE_PUSHv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            u32 w0 = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            u32 cmp0 = ctx.builder.f_ord_equal(ctx.bool_type, w0, zero);
            u32 cmp1 = ctx.builder.f_ord_not_equal(ctx.bool_type, w1, zero);
            u32 cmp = ctx.builder.logical_and(ctx.bool_type, cmp0, cmp1);
            ctx.builder.store(ctx.predicate_var, cmp);
            
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 is_zero = ctx.builder.f_ord_equal(ctx.bool_type, x0, zero);
            u32 inc = ctx.builder.f_add(ctx.float_type, x0, one);
            u32 result = ctx.builder.select(ctx.float_type, is_zero, zero, inc);
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::PRED_SETGT_PUSHv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            u32 w0 = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            u32 cmp0 = ctx.builder.f_ord_equal(ctx.bool_type, w0, zero);
            u32 cmp1 = ctx.builder.f_ord_greater_than(ctx.bool_type, w1, zero);
            u32 cmp = ctx.builder.logical_and(ctx.bool_type, cmp0, cmp1);
            ctx.builder.store(ctx.predicate_var, cmp);
            
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 is_zero = ctx.builder.f_ord_equal(ctx.bool_type, x0, zero);
            u32 inc = ctx.builder.f_add(ctx.float_type, x0, one);
            u32 result = ctx.builder.select(ctx.float_type, is_zero, zero, inc);
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::PRED_SETGTE_PUSHv: {
            u32 zero = ctx.builder.const_float(0.0f);
            u32 one = ctx.builder.const_float(1.0f);
            
            u32 w0 = ctx.builder.composite_extract(ctx.float_type, src0, {3});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            u32 cmp0 = ctx.builder.f_ord_equal(ctx.bool_type, w0, zero);
            u32 cmp1 = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, w1, zero);
            u32 cmp = ctx.builder.logical_and(ctx.bool_type, cmp0, cmp1);
            ctx.builder.store(ctx.predicate_var, cmp);
            
            u32 x0 = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 is_zero = ctx.builder.f_ord_equal(ctx.bool_type, x0, zero);
            u32 inc = ctx.builder.f_add(ctx.float_type, x0, one);
            u32 result = ctx.builder.select(ctx.float_type, is_zero, zero, inc);
            
            return ctx.builder.composite_construct(type, {result, result, result, result});
        }
            
        case AluVectorOp::KILLEv: {
            // Kill if any component equal
            u32 zero = ctx.builder.const_float(0.0f);
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_equal(ctx.bool_type, a, b);
                emit_kill(ctx, cmp);
            }
            return ctx.builder.composite_construct(type, {zero, zero, zero, zero});
        }
            
        case AluVectorOp::KILLGTv: {
            u32 zero = ctx.builder.const_float(0.0f);
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_greater_than(ctx.bool_type, a, b);
                emit_kill(ctx, cmp);
            }
            return ctx.builder.composite_construct(type, {zero, zero, zero, zero});
        }
            
        case AluVectorOp::KILLGTEv: {
            u32 zero = ctx.builder.const_float(0.0f);
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_greater_than_equal(ctx.bool_type, a, b);
                emit_kill(ctx, cmp);
            }
            return ctx.builder.composite_construct(type, {zero, zero, zero, zero});
        }
            
        case AluVectorOp::KILLNEv: {
            u32 zero = ctx.builder.const_float(0.0f);
            for (int i = 0; i < 4; i++) {
                u32 a = ctx.builder.composite_extract(ctx.float_type, src0, {static_cast<u32>(i)});
                u32 b = ctx.builder.composite_extract(ctx.float_type, src1, {static_cast<u32>(i)});
                u32 cmp = ctx.builder.f_ord_not_equal(ctx.bool_type, a, b);
                emit_kill(ctx, cmp);
            }
            return ctx.builder.composite_construct(type, {zero, zero, zero, zero});
        }
            
        case AluVectorOp::DSTv: {
            // Distance vector: result = (1, src0.y * src1.y, src0.z, src1.w)
            u32 one = ctx.builder.const_float(1.0f);
            u32 y0 = ctx.builder.composite_extract(ctx.float_type, src0, {1});
            u32 y1 = ctx.builder.composite_extract(ctx.float_type, src1, {1});
            u32 z0 = ctx.builder.composite_extract(ctx.float_type, src0, {2});
            u32 w1 = ctx.builder.composite_extract(ctx.float_type, src1, {3});
            
            u32 y_mul = ctx.builder.f_mul(ctx.float_type, y0, y1);
            return ctx.builder.composite_construct(type, {one, y_mul, z0, w1});
        }
            
        case AluVectorOp::MOVAv: {
            // Move to address register (from x component)
            u32 x = ctx.builder.composite_extract(ctx.float_type, src0, {0});
            u32 int_val = ctx.builder.convert_f_to_s(ctx.int_type, x);
            ctx.builder.store(ctx.address_reg_var, int_val);
            return src0;
        }
            
        default:
            // Unknown op - return first source
            return src0;
    }
}

u32 ShaderTranslator::get_source_register(TranslationContext& ctx, u8 reg, bool abs, bool negate) {
    // Load from temporary register
    u32 value = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[reg & 0x7F]);
    
    // Apply absolute value if needed
    if (abs) {
        value = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext, spv::GLSLstd450FAbs, {value});
    }
    
    // Apply negation if needed
    if (negate) {
        value = ctx.builder.f_negate(ctx.vec4_type, value);
    }
    
    return value;
}

void ShaderTranslator::write_dest_register(TranslationContext& ctx, u8 reg, u32 value, u8 write_mask) {
    if (write_mask == 0xF) {
        // Write all components
        ctx.builder.store(ctx.temp_vars[reg & 0x7F], value);
    } else if (write_mask != 0) {
        // Partial write - need to merge with existing value
        u32 existing = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[reg & 0x7F]);
        
        std::vector<u32> components;
        for (int i = 0; i < 4; i++) {
            if (write_mask & (1 << i)) {
                components.push_back(ctx.builder.composite_extract(ctx.float_type, value, {static_cast<u32>(i)}));
            } else {
                components.push_back(ctx.builder.composite_extract(ctx.float_type, existing, {static_cast<u32>(i)}));
            }
        }
        
        u32 merged = ctx.builder.composite_construct(ctx.vec4_type, components);
        ctx.builder.store(ctx.temp_vars[reg & 0x7F], merged);
    }
}

void ShaderTranslator::write_dest_with_saturate(TranslationContext& ctx, u8 reg, u32 value, 
                                                 u8 write_mask, bool saturate) {
    if (saturate) {
        value = apply_saturate(ctx, value);
    }
    write_dest_register(ctx, reg, value, write_mask);
}

u32 ShaderTranslator::apply_saturate(TranslationContext& ctx, u32 value) {
    u32 zero = ctx.builder.const_float(0.0f);
    u32 one = ctx.builder.const_float(1.0f);
    u32 zero_vec = ctx.builder.composite_construct(ctx.vec4_type, {zero, zero, zero, zero});
    u32 one_vec = ctx.builder.composite_construct(ctx.vec4_type, {one, one, one, one});
    return ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext, spv::GLSLstd450FClamp, {value, zero_vec, one_vec});
}

void ShaderTranslator::emit_kill(TranslationContext& ctx, u32 condition) {
    // Only emit kill for pixel shaders
    if (ctx.type != ShaderType::Pixel) return;
    
    // Create labels for conditional kill
    u32 kill_label = ctx.builder.allocate_id();
    u32 continue_label = ctx.builder.allocate_id();
    
    ctx.builder.selection_merge(continue_label, 0);
    ctx.builder.branch_conditional(condition, kill_label, continue_label);
    
    ctx.builder.label(kill_label);
    ctx.builder.kill();
    
    ctx.builder.label(continue_label);
}

u32 ShaderTranslator::emit_cube_vector(TranslationContext& ctx, u32 src0, u32 src1) {
    // Cube map coordinate generation
    // Returns (tc, sc, ma, face_id) for cube map sampling
    
    u32 x = ctx.builder.composite_extract(ctx.float_type, src0, {0});
    u32 y = ctx.builder.composite_extract(ctx.float_type, src0, {1});
    u32 z = ctx.builder.composite_extract(ctx.float_type, src0, {2});
    
    // Get absolute values
    u32 ax = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FAbs, {x});
    u32 ay = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FAbs, {y});
    u32 az = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FAbs, {z});
    
    // Find the major axis
    u32 max_xy = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FMax, {ax, ay});
    u32 ma = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext, spv::GLSLstd450FMax, {max_xy, az});
    
    // Compute texture coordinates (simplified - full impl would branch on major axis)
    // For now, assume Z is the major axis (front/back faces)
    u32 two = ctx.builder.const_float(2.0f);
    u32 ma2 = ctx.builder.f_mul(ctx.float_type, ma, two);
    u32 inv_ma2 = ctx.builder.f_div(ctx.float_type, ctx.builder.const_float(1.0f), ma2);
    
    u32 tc = ctx.builder.f_mul(ctx.float_type, x, inv_ma2);
    u32 sc = ctx.builder.f_mul(ctx.float_type, y, inv_ma2);
    
    // Face ID (simplified - would need proper face selection)
    u32 face = ctx.builder.const_float(0.0f);
    
    return ctx.builder.composite_construct(ctx.vec4_type, {tc, sc, ma, face});
}

u32 ShaderTranslator::get_constant(TranslationContext& ctx, u32 index) {
    // Load from constant buffer
    u32 const_idx = ctx.builder.const_uint(index);
    u32 vec4_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, ctx.vec4_type);
    
    u32 constants_var = ctx.type == ShaderType::Vertex ? 
                        ctx.vertex_constants_var : ctx.pixel_constants_var;
    u32 zero_idx = ctx.builder.const_uint(0);
    u32 ptr = ctx.builder.access_chain(vec4_ptr, constants_var, {zero_idx, const_idx});
    return ctx.builder.load(ctx.vec4_type, ptr);
}

u32 ShaderTranslator::get_bool_constant(TranslationContext& ctx, u32 index) {
    // Load from bool constant buffer (as uint, then convert)
    if (ctx.bool_constants_var == 0) {
        return ctx.builder.const_bool(false);
    }
    
    u32 const_idx = ctx.builder.const_uint(index / 32);
    u32 bit_idx = index % 32;
    
    u32 uint_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, ctx.uint_type);
    u32 zero_idx = ctx.builder.const_uint(0);
    u32 ptr = ctx.builder.access_chain(uint_ptr, ctx.bool_constants_var, {zero_idx, const_idx});
    u32 word = ctx.builder.load(ctx.uint_type, ptr);
    
    // Extract bit
    u32 shifted = ctx.builder.i_sub(ctx.uint_type, word, ctx.builder.const_uint(bit_idx));
    u32 masked = ctx.builder.bitcast(ctx.uint_type, shifted);  // Would need proper AND
    u32 cmp = ctx.builder.i_not_equal(ctx.bool_type, masked, ctx.builder.const_uint(0));
    
    return cmp;
}

u32 ShaderTranslator::get_loop_constant(TranslationContext& ctx, u32 index) {
    // Loop constants contain (count, start, step, _)
    if (ctx.loop_constants_var == 0) {
        return ctx.builder.composite_construct(ctx.uvec4_type, {
            ctx.builder.const_uint(0),
            ctx.builder.const_uint(0),
            ctx.builder.const_uint(1),
            ctx.builder.const_uint(0)
        });
    }
    
    u32 const_idx = ctx.builder.const_uint(index);
    u32 uvec4_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, ctx.uvec4_type);
    u32 zero_idx = ctx.builder.const_uint(0);
    u32 ptr = ctx.builder.access_chain(uvec4_ptr, ctx.loop_constants_var, {zero_idx, const_idx});
    return ctx.builder.load(ctx.uvec4_type, ptr);
}

u32 ShaderTranslator::apply_swizzle(TranslationContext& ctx, u32 value, u8 swizzle) {
    // Swizzle is packed as 2 bits per component: xxyyzzww
    u32 x_idx = (swizzle >> 0) & 0x3;
    u32 y_idx = (swizzle >> 2) & 0x3;
    u32 z_idx = (swizzle >> 4) & 0x3;
    u32 w_idx = (swizzle >> 6) & 0x3;
    
    // If identity swizzle, return as-is
    if (x_idx == 0 && y_idx == 1 && z_idx == 2 && w_idx == 3) {
        return value;
    }
    
    return ctx.builder.vector_shuffle(ctx.vec4_type, value, value, {x_idx, y_idx, z_idx, w_idx});
}

u32 ShaderTranslator::get_source_with_swizzle(TranslationContext& ctx, u8 reg, u8 swizzle, 
                                               bool abs, bool negate, bool is_const) {
    u32 value;
    
    if (is_const) {
        // Load from constant buffer
        value = get_constant(ctx, reg);
    } else {
        // Load from temporary register
        value = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[reg & 0x7F]);
    }
    
    // Apply swizzle
    if (swizzle != 0xE4) {  // 0xE4 = identity swizzle (xyzw)
        value = apply_swizzle(ctx, value, swizzle);
    }
    
    // Apply absolute value
    if (abs) {
        value = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext, spv::GLSLstd450FAbs, {value});
    }
    
    // Apply negation
    if (negate) {
        value = ctx.builder.f_negate(ctx.vec4_type, value);
    }
    
    return value;
}

void ShaderTranslator::handle_export(TranslationContext& ctx, u32 value, u8 export_type, u8 export_index) {
    switch (static_cast<ExportType>(export_type)) {
        case ExportType::Position:
            write_position(ctx, value);
            break;
        case ExportType::Interpolator:
            write_interpolant(ctx, value, export_index);
            break;
        case ExportType::PointSize:
            if (ctx.point_size_var != 0) {
                u32 size = ctx.builder.composite_extract(ctx.float_type, value, {0});
                ctx.builder.store(ctx.point_size_var, size);
            }
            break;
        case ExportType::Color:
            write_color(ctx, value, export_index);
            break;
        case ExportType::Depth:
            if (ctx.frag_depth_var != 0) {
                u32 depth = ctx.builder.composite_extract(ctx.float_type, value, {0});
                ctx.builder.store(ctx.frag_depth_var, depth);
            }
            break;
    }
}

void ShaderTranslator::write_position(TranslationContext& ctx, u32 value) {
    if (ctx.position_var != 0) {
        ctx.builder.store(ctx.position_var, value);
    }
}

void ShaderTranslator::write_interpolant(TranslationContext& ctx, u32 value, u8 index) {
    if (index < 16 && ctx.interpolant_vars[index] != 0) {
        ctx.builder.store(ctx.interpolant_vars[index], value);
    }
}

void ShaderTranslator::write_color(TranslationContext& ctx, u32 value, u8 index) {
    if (index < 4 && ctx.color_outputs[index] != 0) {
        ctx.builder.store(ctx.color_outputs[index], value);
    } else if (ctx.frag_color_var != 0) {
        ctx.builder.store(ctx.frag_color_var, value);
    }
}

u32 ShaderTranslator::translate_texture_fetch(TranslationContext& ctx, 
                                               const ShaderMicrocode::FetchInstruction& inst) {
    u8 sampler_idx = inst.const_index & 0xF;
    
    // Get texture coordinate from source register
    u32 src_val = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[inst.src_reg & 0x7F]);
    
    // Get appropriate coordinate based on dimension
    u32 coord;
    u32 result_type = ctx.vec4_type;
    
    switch (inst.dimension) {
        case TextureDimension::k1D: {
            coord = ctx.builder.composite_extract(ctx.float_type, src_val, {0});
            break;
        }
        case TextureDimension::k2D: {
            u32 x = ctx.builder.composite_extract(ctx.float_type, src_val, {0});
            u32 y = ctx.builder.composite_extract(ctx.float_type, src_val, {1});
            coord = ctx.builder.composite_construct(ctx.vec2_type, {x, y});
            break;
        }
        case TextureDimension::k3D: {
            u32 x = ctx.builder.composite_extract(ctx.float_type, src_val, {0});
            u32 y = ctx.builder.composite_extract(ctx.float_type, src_val, {1});
            u32 z = ctx.builder.composite_extract(ctx.float_type, src_val, {2});
            coord = ctx.builder.composite_construct(ctx.vec3_type, {x, y, z});
            break;
        }
        case TextureDimension::kCube: {
            u32 x = ctx.builder.composite_extract(ctx.float_type, src_val, {0});
            u32 y = ctx.builder.composite_extract(ctx.float_type, src_val, {1});
            u32 z = ctx.builder.composite_extract(ctx.float_type, src_val, {2});
            coord = ctx.builder.composite_construct(ctx.vec3_type, {x, y, z});
            break;
        }
        default: {
            u32 x = ctx.builder.composite_extract(ctx.float_type, src_val, {0});
            u32 y = ctx.builder.composite_extract(ctx.float_type, src_val, {1});
            coord = ctx.builder.composite_construct(ctx.vec2_type, {x, y});
            break;
        }
    }
    
    // Sample the texture
    u32 result;
    if (ctx.sampler_vars[sampler_idx] != 0 && ctx.texture_vars[sampler_idx] != 0) {
        // Create sampled image
        u32 sampled_img = ctx.builder.sampled_image(
            ctx.sampled_image_types[sampler_idx],
            ctx.texture_vars[sampler_idx],
            ctx.sampler_vars[sampler_idx]
        );
        
        // Sample with optional LOD bias
        if (inst.use_register_lod) {
            u32 lod = ctx.builder.composite_extract(ctx.float_type, src_val, {3});
            result = ctx.builder.image_sample_lod(result_type, sampled_img, coord, lod);
        } else if (inst.lod_bias != 0.0f) {
            u32 bias = ctx.builder.const_float(inst.lod_bias);
            result = ctx.builder.image_sample(result_type, sampled_img, coord, bias);
        } else {
            result = ctx.builder.image_sample(result_type, sampled_img, coord);
        }
    } else {
        // No texture bound - return default color
        u32 one = ctx.builder.const_float(1.0f);
        u32 half = ctx.builder.const_float(0.5f);
        result = ctx.builder.composite_construct(ctx.vec4_type, {half, half, half, one});
    }
    
    return result;
}

u32 ShaderTranslator::translate_vertex_fetch(TranslationContext& ctx,
                                              const ShaderMicrocode::FetchInstruction& inst) {
    // Vertex fetch from vertex buffer
    u8 slot = inst.const_index & 0xF;
    
    if (ctx.vertex_input_vars[slot] != 0) {
        // Load from vertex input
        return ctx.builder.load(ctx.vec4_type, ctx.vertex_input_vars[slot]);
    } else {
        // Fallback: load from constant buffer
        u32 const_val = get_constant(ctx, inst.const_index);
        return const_val;
    }
}

ShaderInfo ShaderTranslator::analyze(const void* microcode, u32 size, ShaderType type) {
    ShaderInfo info{};
    info.type = type;
    
    ShaderMicrocode parsed;
    if (parsed.parse(microcode, size, type) != Status::Ok) {
        return info;
    }
    
    info.instruction_count = parsed.instruction_count();
    
    // Analyze ALU instructions
    u32 max_temp = 0;
    u32 max_const = 0;
    
    for (const auto& alu : parsed.alu_instructions()) {
        // Track register usage
        max_temp = std::max(max_temp, static_cast<u32>(alu.dest_reg & 0x7F));
        for (int i = 0; i < 3; i++) {
            if (!alu.src_is_const[i]) {
                max_temp = std::max(max_temp, static_cast<u32>(alu.src_regs[i] & 0x7F));
            } else {
                max_const = std::max(max_const, static_cast<u32>(alu.src_regs[i]));
            }
        }
        
        // Check for predication
        if (alu.predicated) {
            info.uses_predication = true;
        }
        
        // Check for exports
        if (alu.export_data) {
            switch (static_cast<ExportType>(alu.export_type)) {
                case ExportType::Position:
                    info.exports_position = true;
                    break;
                case ExportType::PointSize:
                    info.exports_point_size = true;
                    break;
                case ExportType::Color:
                    info.color_export_count = std::max(info.color_export_count, 
                                                       static_cast<u32>(alu.export_index + 1));
                    break;
                case ExportType::Depth:
                    info.exports_depth = true;
                    break;
                case ExportType::Interpolator:
                    // Track interpolant
                    if (alu.export_index < 16) {
                        bool found = false;
                        for (const auto& interp : info.interpolants) {
                            if (interp.index == alu.export_index) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            info.interpolants.push_back({alu.export_index, 4, false, true});
                        }
                    }
                    break;
            }
        }
        
        // Check for kill instructions
        AluVectorOp vec_op = static_cast<AluVectorOp>(alu.vector_opcode);
        AluScalarOp scalar_op = static_cast<AluScalarOp>(alu.scalar_opcode);
        
        if (vec_op >= AluVectorOp::KILLEv && vec_op <= AluVectorOp::KILLNEv) {
            info.uses_kill = true;
        }
        if (scalar_op >= AluScalarOp::KILLEs && scalar_op <= AluScalarOp::KILLONEs) {
            info.uses_kill = true;
        }
    }
    
    // Analyze fetch instructions
    for (const auto& fetch : parsed.fetch_instructions()) {
        if (fetch.fetch_type == 0) {
            // Vertex fetch
            u32 slot = fetch.const_index & 0xF;
            bool found = false;
            for (u32 s : info.vertex_fetch_slots) {
                if (s == slot) { found = true; break; }
            }
            if (!found) {
                info.vertex_fetch_slots.push_back(slot);
            }
        } else {
            // Texture fetch
            u32 binding = fetch.const_index & 0xF;
            bool found = false;
            for (u32 b : info.texture_bindings) {
                if (b == binding) { found = true; break; }
            }
            if (!found) {
                info.texture_bindings.push_back(binding);
                info.texture_dimensions.push_back(fetch.dimension);
            }
        }
    }
    
    // Analyze control flow
    for (const auto& cf : parsed.cf_instructions()) {
        ControlFlowOp op = static_cast<ControlFlowOp>(cf.opcode);
        if (op == ControlFlowOp::LOOP_START) {
            info.loop_count++;
        }
        if (op == ControlFlowOp::COND_EXEC || op == ControlFlowOp::COND_EXEC_END ||
            op == ControlFlowOp::COND_PRED_EXEC || op == ControlFlowOp::COND_PRED_EXEC_END) {
            info.conditional_count++;
        }
    }
    
    info.temp_register_count = max_temp + 1;
    info.max_const_register = max_const;
    
    return info;
}

void ShaderTranslator::setup_constants(TranslationContext& ctx) {
    // Cache commonly used constants
    ctx.const_zero = ctx.builder.const_float(0.0f);
    ctx.const_one = ctx.builder.const_float(1.0f);
    ctx.const_neg_one = ctx.builder.const_float(-1.0f);
    ctx.const_half = ctx.builder.const_float(0.5f);
    ctx.const_two = ctx.builder.const_float(2.0f);
    
    ctx.const_vec4_zero = ctx.builder.composite_construct(ctx.vec4_type, {
        ctx.const_zero, ctx.const_zero, ctx.const_zero, ctx.const_zero
    });
    ctx.const_vec4_one = ctx.builder.composite_construct(ctx.vec4_type, {
        ctx.const_one, ctx.const_one, ctx.const_one, ctx.const_one
    });
}

void ShaderTranslator::setup_temporaries(TranslationContext& ctx) {
    // Initialize temporary registers
    u32 ptr_type = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.vec4_type);
    for (u32 i = 0; i < 128; i++) {
        ctx.temp_vars[i] = ctx.builder.variable(ptr_type, spv::StorageClassFunction);
    }
    
    // Initialize predicate register
    u32 bool_ptr_type = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.bool_type);
    ctx.predicate_var = ctx.builder.variable(bool_ptr_type, spv::StorageClassFunction);
    
    // Initialize address register
    u32 int_ptr_type = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.int_type);
    ctx.address_reg_var = ctx.builder.variable(int_ptr_type, spv::StorageClassFunction);
    
    // Initialize previous result storage
    ctx.prev_scalar = 0;
    ctx.prev_vector = 0;
}

void ShaderTranslator::setup_samplers(TranslationContext& ctx, const ShaderInfo& info) {
    if (info.texture_bindings.empty()) return;
    
    for (size_t i = 0; i < info.texture_bindings.size(); i++) {
        u32 binding = info.texture_bindings[i];
        TextureDimension dim = i < info.texture_dimensions.size() ? 
                               info.texture_dimensions[i] : TextureDimension::k2D;
        
        // Create image type based on dimension
        u32 spv_dim;
        switch (dim) {
            case TextureDimension::k1D: spv_dim = spv::Dim1D; break;
            case TextureDimension::k3D: spv_dim = spv::Dim3D; break;
            case TextureDimension::kCube: spv_dim = spv::DimCube; break;
            default: spv_dim = spv::Dim2D; break;
        }
        
        u32 image_type = ctx.builder.type_image(ctx.float_type, spv_dim, false, false, false, 1);
        u32 sampled_image_type = ctx.builder.type_sampled_image(image_type);
        ctx.sampled_image_types[binding] = sampled_image_type;
        
        // Create sampler variable
        u32 sampler_type = ctx.builder.type_sampler();
        u32 sampler_ptr = ctx.builder.type_pointer(spv::StorageClassUniformConstant, sampler_type);
        ctx.sampler_vars[binding] = ctx.builder.variable(sampler_ptr, spv::StorageClassUniformConstant);
        ctx.builder.decorate(ctx.sampler_vars[binding], spv::DecorationDescriptorSet, {1});
        ctx.builder.decorate(ctx.sampler_vars[binding], spv::DecorationBinding, {binding});
        
        // Create texture variable
        u32 image_ptr = ctx.builder.type_pointer(spv::StorageClassUniformConstant, image_type);
        ctx.texture_vars[binding] = ctx.builder.variable(image_ptr, spv::StorageClassUniformConstant);
        ctx.builder.decorate(ctx.texture_vars[binding], spv::DecorationDescriptorSet, {1});
        ctx.builder.decorate(ctx.texture_vars[binding], spv::DecorationBinding, {binding + 16});
    }
}

void ShaderTranslator::setup_interpolants(TranslationContext& ctx, const ShaderInfo& info) {
    u32 vec4_ptr;
    u32 storage_class;
    
    if (ctx.type == ShaderType::Vertex) {
        storage_class = spv::StorageClassOutput;
    } else {
        storage_class = spv::StorageClassInput;
    }
    
    vec4_ptr = ctx.builder.type_pointer(storage_class, ctx.vec4_type);
    
    for (const auto& interp : info.interpolants) {
        if (interp.index >= 16) continue;
        
        ctx.interpolant_vars[interp.index] = ctx.builder.variable(vec4_ptr, storage_class);
        ctx.builder.decorate(ctx.interpolant_vars[interp.index], spv::DecorationLocation, {interp.index});
        
        if (interp.flat) {
            ctx.builder.decorate(ctx.interpolant_vars[interp.index], spv::DecorationFlat);
        }
    }
}

} // namespace x360mu
