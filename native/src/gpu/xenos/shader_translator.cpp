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
        return *cached;
    }
    
    // Parse the microcode
    ShaderMicrocode parsed;
    if (parsed.parse(microcode, size, type) != Status::Ok) {
        LOGE("Failed to parse shader microcode");
        return {};
    }
    
    // Create translation context
    TranslationContext ctx;
    ctx.type = type;
    
    // Begin SPIR-V generation
    ctx.builder.begin(type);
    
    // Import GLSL.std.450 extension
    ctx.glsl_ext = ctx.builder.import_extension("GLSL.std.450");
    
    // Setup types
    setup_types(ctx);
    
    // Setup inputs/outputs based on shader type
    setup_inputs(ctx);
    setup_outputs(ctx);
    setup_uniforms(ctx);
    
    // Create main function
    u32 void_func_type = ctx.builder.type_function(ctx.void_type, {});
    ctx.builder.function_begin(ctx.void_type, void_func_type);
    
    u32 entry_label = ctx.builder.allocate_id();
    ctx.builder.label(entry_label);
    
    // Initialize temporary registers
    for (u32 i = 0; i < 128; i++) {
        u32 ptr_type = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.vec4_type);
        ctx.temp_vars[i] = ctx.builder.variable(ptr_type, spv::StorageClassFunction);
    }
    
    // Initialize predicate register
    u32 bool_ptr_type = ctx.builder.type_pointer(spv::StorageClassFunction, ctx.bool_type);
    ctx.predicate_var = ctx.builder.variable(bool_ptr_type, spv::StorageClassFunction);
    
    // Translate shader body
    translate_control_flow(ctx, parsed);
    
    // Write output
    if (type == ShaderType::Vertex) {
        // Export position
        u32 pos = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[0]);
        ctx.builder.store(ctx.position_var, pos);
    } else {
        // Export fragment color
        u32 color = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[0]);
        ctx.builder.store(ctx.frag_color_var, color);
    }
    
    ctx.builder.return_void();
    ctx.builder.function_end();
    
    // Add entry point
    std::vector<u32> interface_vars;
    if (type == ShaderType::Vertex) {
        interface_vars = {ctx.position_var, ctx.vertex_id_var};
    } else {
        interface_vars = {ctx.frag_coord_var, ctx.frag_color_var};
    }
    
    u32 entry_func = 4;  // Main function ID
    ctx.builder.entry_point(
        type == ShaderType::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment,
        entry_func, "main", interface_vars);
    
    if (type == ShaderType::Pixel) {
        ctx.builder.execution_mode(entry_func, spv::ExecutionModeOriginUpperLeft);
    }
    
    // Generate final SPIR-V
    std::vector<u32> spirv = ctx.builder.end();
    
    // Cache the result
    cache(hash, spirv);
    
    LOGD("Translated %s shader: %u bytes microcode -> %zu words SPIR-V",
         type == ShaderType::Vertex ? "vertex" : "pixel",
         size, spirv.size());
    
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
    ctx.mat4_type = ctx.builder.type_matrix(ctx.vec4_type, 4);
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
    // Get source values
    std::vector<u32> sources;
    for (int i = 0; i < 3; i++) {
        u32 src = get_source_register(ctx, inst.src_regs[i], inst.abs[i], inst.negate[i]);
        sources.push_back(src);
    }
    
    // Execute vector operation
    u32 vector_result = 0;
    if (inst.vector_opcode != 0 || inst.write_mask != 0) {
        vector_result = translate_vector_op(ctx, static_cast<AluVectorOp>(inst.vector_opcode), 
                                           sources, ctx.vec4_type);
    }
    
    // Execute scalar operation  
    u32 scalar_result = 0;
    if (inst.scalar_opcode != 0) {
        // Scalar ops use the w component of source
        u32 scalar_src = ctx.builder.composite_extract(ctx.float_type, sources[0], {3});
        scalar_result = translate_scalar_op(ctx, static_cast<AluScalarOp>(inst.scalar_opcode),
                                           scalar_src, ctx.float_type);
    }
    
    // Combine results and write to destination
    u32 final_result = vector_result;
    if (scalar_result != 0 && vector_result != 0) {
        // Replace W component with scalar result
        final_result = ctx.builder.composite_insert(ctx.vec4_type, scalar_result, vector_result, {3});
    } else if (scalar_result != 0) {
        // Broadcast scalar to all components
        final_result = ctx.builder.composite_construct(ctx.vec4_type, 
            {scalar_result, scalar_result, scalar_result, scalar_result});
    }
    
    if (final_result != 0) {
        write_dest_register(ctx, inst.dest_reg, final_result, inst.write_mask);
    }
}

void ShaderTranslator::translate_fetch_instruction(TranslationContext& ctx,
                                                    const ShaderMicrocode::FetchInstruction& inst) {
    if (inst.fetch_type == 0) {
        // Vertex fetch - load from vertex buffer
        // For now, just load from constants as placeholder
        u32 const_index = ctx.builder.const_uint(inst.const_index);
        u32 vec4_ptr = ctx.builder.type_pointer(spv::StorageClassUniform, ctx.vec4_type);
        
        u32 constants_var = ctx.type == ShaderType::Vertex ? 
                            ctx.vertex_constants_var : ctx.pixel_constants_var;
        u32 ptr = ctx.builder.access_chain(vec4_ptr, constants_var, 
                                          {ctx.builder.const_uint(0), const_index});
        u32 value = ctx.builder.load(ctx.vec4_type, ptr);
        
        ctx.builder.store(ctx.temp_vars[inst.dest_reg & 0x7F], value);
    } else {
        // Texture fetch
        // TODO: Implement texture sampling
        // For now, return a default color
        u32 one = ctx.builder.const_float(1.0f);
        u32 half = ctx.builder.const_float(0.5f);
        u32 color = ctx.builder.composite_construct(ctx.vec4_type, {half, half, half, one});
        ctx.builder.store(ctx.temp_vars[inst.dest_reg & 0x7F], color);
    }
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
    } else {
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

} // namespace x360mu
