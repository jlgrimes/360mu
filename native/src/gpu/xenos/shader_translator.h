/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos Shader Translator
 * Converts Xenos GPU shader microcode to SPIR-V for Vulkan
 */

#pragma once

#include "x360mu/types.h"
#include "gpu.h"
#include "command_processor.h"
#include <vector>
#include <unordered_map>
#include <string>

namespace x360mu {

/**
 * Xenos ALU scalar opcodes
 */
enum class AluScalarOp : u8 {
    ADDs = 0,
    ADD_PREVs = 1,
    MULs = 2,
    MUL_PREVs = 3,
    MUL_PREV2s = 4,
    MAXs = 5,
    MINs = 6,
    SETEs = 7,
    SETGTs = 8,
    SETGTEs = 9,
    SETNEs = 10,
    FRACs = 11,
    TRUNCs = 12,
    FLOORs = 13,
    EXP_IEEE = 14,
    LOG_CLAMP = 15,
    LOG_IEEE = 16,
    RECIP_CLAMP = 17,
    RECIP_FF = 18,
    RECIP_IEEE = 19,
    RECIPSQ_CLAMP = 20,
    RECIPSQ_FF = 21,
    RECIPSQ_IEEE = 22,
    MOVAs = 23,
    MOVA_FLOORs = 24,
    SUBs = 25,
    SUB_PREVs = 26,
    PRED_SETEs = 27,
    PRED_SETNEs = 28,
    PRED_SETGTs = 29,
    PRED_SETGTEs = 30,
    PRED_SET_INVs = 31,
    PRED_SET_POPs = 32,
    PRED_SET_CLRs = 33,
    PRED_SET_RESTOREs = 34,
    KILLEs = 35,
    KILLGTs = 36,
    KILLGTEs = 37,
    KILLNEs = 38,
    KILLONEs = 39,
    SQRT_IEEE = 40,
    MUL_CONST_0 = 42,
    MUL_CONST_1 = 43,
    ADD_CONST_0 = 44,
    ADD_CONST_1 = 45,
    SUB_CONST_0 = 46,
    SUB_CONST_1 = 47,
    SIN = 48,
    COS = 49,
    RETAIN_PREV = 50,
};

/**
 * Xenos ALU vector opcodes
 */
enum class AluVectorOp : u8 {
    ADDv = 0,
    MULv = 1,
    MAXv = 2,
    MINv = 3,
    SETEv = 4,
    SETGTv = 5,
    SETGTEv = 6,
    SETNEv = 7,
    FRACv = 8,
    TRUNCv = 9,
    FLOORv = 10,
    MULADDv = 11,
    CNDEv = 12,
    CNDGTEv = 13,
    CNDGTv = 14,
    DOT4v = 15,
    DOT3v = 16,
    DOT2ADDv = 17,
    CUBEv = 18,
    MAX4v = 19,
    PRED_SETE_PUSHv = 20,
    PRED_SETNE_PUSHv = 21,
    PRED_SETGT_PUSHv = 22,
    PRED_SETGTE_PUSHv = 23,
    KILLEv = 24,
    KILLGTv = 25,
    KILLGTEv = 26,
    KILLNEv = 27,
    DSTv = 28,
    MOVAv = 29,
};

/**
 * Xenos fetch opcodes
 */
enum class FetchOp : u8 {
    VertexFetch = 0,
    TextureFetch = 1,
    GetTextureBorderColorFrac = 16,
    GetTextureComputedLod = 17,
    GetTextureGradients = 18,
    GetTextureWeights = 19,
    SetTextureLod = 24,
    SetTextureGradientsH = 25,
    SetTextureGradientsV = 26,
    UnknownTextureOp = 27,
};

/**
 * SPIR-V builder for shader translation
 */
class SpirvBuilder {
public:
    SpirvBuilder();
    
    /**
     * Begin building a shader
     */
    void begin(ShaderType type);
    
    /**
     * End building and return SPIR-V binary
     */
    std::vector<u32> end();
    
    // Type declarations
    u32 type_void();
    u32 type_bool();
    u32 type_int(u32 width, bool signed_);
    u32 type_float(u32 width);
    u32 type_vector(u32 component_type, u32 count);
    u32 type_matrix(u32 column_type, u32 columns);
    u32 type_array(u32 element_type, u32 length);
    u32 type_struct(const std::vector<u32>& members);
    u32 type_pointer(u32 storage_class, u32 type);
    u32 type_function(u32 return_type, const std::vector<u32>& params);
    u32 type_image(u32 sampled_type, u32 dim, bool depth, bool arrayed, bool ms, u32 sampled);
    u32 type_sampled_image(u32 image_type);
    
    // Constants
    u32 const_bool(bool value);
    u32 const_int(s32 value);
    u32 const_uint(u32 value);
    u32 const_float(f32 value);
    u32 const_composite(u32 type, const std::vector<u32>& constituents);
    
    // Variables
    u32 variable(u32 pointer_type, u32 storage_class, u32 initializer = 0);
    
    // Function
    void function_begin(u32 return_type, u32 function_type);
    void function_end();
    void label(u32 id);
    void return_void();
    void return_value(u32 value);
    
    // Memory operations
    u32 load(u32 result_type, u32 pointer);
    void store(u32 pointer, u32 value);
    u32 access_chain(u32 result_type, u32 base, const std::vector<u32>& indices);
    
    // Arithmetic
    u32 f_add(u32 type, u32 a, u32 b);
    u32 f_sub(u32 type, u32 a, u32 b);
    u32 f_mul(u32 type, u32 a, u32 b);
    u32 f_div(u32 type, u32 a, u32 b);
    u32 f_negate(u32 type, u32 a);
    u32 f_mod(u32 type, u32 a, u32 b);
    
    // Math functions
    u32 ext_inst(u32 type, u32 set, u32 instruction, const std::vector<u32>& operands);
    
    // Vector operations
    u32 vector_shuffle(u32 type, u32 v1, u32 v2, const std::vector<u32>& components);
    u32 composite_extract(u32 type, u32 composite, const std::vector<u32>& indices);
    u32 composite_insert(u32 type, u32 object, u32 composite, const std::vector<u32>& indices);
    u32 composite_construct(u32 type, const std::vector<u32>& constituents);
    
    // Comparison
    u32 f_ord_equal(u32 type, u32 a, u32 b);
    u32 f_ord_not_equal(u32 type, u32 a, u32 b);
    u32 f_ord_less_than(u32 type, u32 a, u32 b);
    u32 f_ord_greater_than(u32 type, u32 a, u32 b);
    u32 f_ord_less_than_equal(u32 type, u32 a, u32 b);
    u32 f_ord_greater_than_equal(u32 type, u32 a, u32 b);
    
    // Control flow
    u32 select(u32 type, u32 condition, u32 true_val, u32 false_val);
    void branch(u32 target);
    void branch_conditional(u32 condition, u32 true_label, u32 false_label);
    void loop_merge(u32 merge_block, u32 continue_target, u32 control);
    void selection_merge(u32 merge_block, u32 control);
    
    // Texture operations  
    u32 image_sample(u32 type, u32 sampled_image, u32 coord, u32 bias = 0);
    
    // Decorations
    void decorate(u32 target, u32 decoration, const std::vector<u32>& operands = {});
    void member_decorate(u32 type, u32 member, u32 decoration, const std::vector<u32>& operands = {});
    
    // Debug
    void name(u32 target, const std::string& name);
    void member_name(u32 type, u32 member, const std::string& name);
    
    // Entry point
    void entry_point(u32 execution_model, u32 entry_point, const std::string& name, 
                    const std::vector<u32>& interface);
    void execution_mode(u32 entry_point, u32 mode, const std::vector<u32>& operands = {});
    
    // Extensions
    u32 import_extension(const std::string& name);
    void capability(u32 cap);
    void memory_model(u32 addressing, u32 memory);
    
    // Get new ID
    u32 allocate_id() { return next_id_++; }
    
private:
    u32 next_id_ = 1;
    
    // SPIR-V sections
    std::vector<u32> capabilities_;
    std::vector<u32> extensions_;
    std::vector<u32> ext_inst_imports_;
    std::vector<u32> memory_model_;
    std::vector<u32> entry_points_;
    std::vector<u32> execution_modes_;
    std::vector<u32> debug_names_;
    std::vector<u32> decorations_;
    std::vector<u32> types_constants_;
    std::vector<u32> globals_;
    std::vector<u32> functions_;
    
    // Current function being built
    std::vector<u32> current_function_;
    
    // Type cache (to avoid duplicates)
    std::unordered_map<u64, u32> type_cache_;
    
    // Emit helpers
    void emit(std::vector<u32>& target, u32 word);
    void emit_op(std::vector<u32>& target, u32 opcode, u32 result_type, u32 result_id, 
                const std::vector<u32>& operands);
};

/**
 * Main shader translator
 */
class ShaderTranslator {
public:
    ShaderTranslator();
    ~ShaderTranslator();
    
    /**
     * Initialize
     */
    Status initialize(const std::string& cache_path);
    
    /**
     * Shutdown
     */
    void shutdown();
    
    /**
     * Translate Xenos shader to SPIR-V
     */
    std::vector<u32> translate(const void* microcode, u32 size, ShaderType type);
    
    /**
     * Get cached shader by hash
     */
    const std::vector<u32>* get_cached(u64 hash) const;
    
    /**
     * Cache a translated shader
     */
    void cache(u64 hash, std::vector<u32> spirv);
    
    /**
     * Save cache to disk
     */
    void save_cache();
    
    /**
     * Load cache from disk
     */
    void load_cache();
    
private:
    std::string cache_path_;
    std::unordered_map<u64, std::vector<u32>> cache_;
    
    // Translation context
    struct TranslationContext {
        SpirvBuilder builder;
        ShaderType type;
        
        // Type IDs
        u32 void_type;
        u32 bool_type;
        u32 int_type;
        u32 uint_type;
        u32 float_type;
        u32 vec2_type;
        u32 vec3_type;
        u32 vec4_type;
        u32 mat4_type;
        
        // Built-in inputs/outputs
        u32 position_var;      // gl_Position
        u32 vertex_id_var;     // gl_VertexIndex
        u32 frag_coord_var;    // gl_FragCoord
        u32 front_facing_var;  // gl_FrontFacing
        u32 frag_color_var;    // Output color
        
        // Uniform blocks
        u32 vertex_constants_var;
        u32 pixel_constants_var;
        u32 bool_constants_var;
        
        // Temporaries (128 available)
        std::array<u32, 128> temp_vars;
        
        // Address register
        u32 address_reg;
        
        // Predicate
        u32 predicate_var;
        
        // GLSL extension
        u32 glsl_ext;
        
        // Entry point
        u32 main_function;
    };
    
    void setup_types(TranslationContext& ctx);
    void setup_inputs(TranslationContext& ctx);
    void setup_outputs(TranslationContext& ctx);
    void setup_uniforms(TranslationContext& ctx);
    
    void translate_control_flow(TranslationContext& ctx, const ShaderMicrocode& microcode);
    void translate_alu_instruction(TranslationContext& ctx, const ShaderMicrocode::AluInstruction& inst);
    void translate_fetch_instruction(TranslationContext& ctx, const ShaderMicrocode::FetchInstruction& inst);
    
    u32 translate_scalar_op(TranslationContext& ctx, AluScalarOp op, u32 src, u32 type);
    u32 translate_vector_op(TranslationContext& ctx, AluVectorOp op, 
                           const std::vector<u32>& sources, u32 type);
    
    u32 get_source_register(TranslationContext& ctx, u8 reg, bool abs, bool negate);
    void write_dest_register(TranslationContext& ctx, u8 reg, u32 value, u8 write_mask);
    
    static u64 compute_hash(const void* data, u32 size);
};

} // namespace x360mu

