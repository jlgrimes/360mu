/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos Shader Translator
 * Converts Xenos GPU shader microcode to SPIR-V for Vulkan
 * 
 * Implements full translation of Xbox 360 Xenos GPU shaders including:
 * - ALU vector operations (30+ opcodes)
 * - ALU scalar operations (50+ opcodes)
 * - Texture fetch operations (tfetch2D, tfetch3D, tfetchCube)
 * - Vertex fetch operations
 * - Control flow (loops, conditionals, predicates)
 * - Swizzle/write mask handling
 * - Export/interpolant handling
 * 
 * Reference: Xenia GPU shader documentation and ATI R5xx documentation
 */

#pragma once

#include "x360mu/types.h"
#include "gpu.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <array>
#include <functional>

namespace x360mu {

// Forward declarations
class ShaderMicrocode;

//=============================================================================
// Xenos Shader Constants
//=============================================================================

/**
 * Maximum registers and constants available in Xenos shaders
 */
constexpr u32 kMaxTempRegisters = 128;
constexpr u32 kMaxFloatConstants = 256;
constexpr u32 kMaxBoolConstants = 256;
constexpr u32 kMaxLoopConstants = 32;
constexpr u32 kMaxTextureSamplers = 16;
constexpr u32 kMaxVertexAttributes = 16;
constexpr u32 kMaxInterpolants = 16;
constexpr u32 kMaxExportRegisters = 4;

//=============================================================================
// Xenos ALU Opcodes
//=============================================================================

/**
 * Xenos ALU scalar opcodes (complete list - 51 opcodes)
 */
enum class AluScalarOp : u8 {
    ADDs = 0,              // Add (single source)
    ADD_PREVs = 1,         // Add with previous result
    MULs = 2,              // Multiply (single source, squares)
    MUL_PREVs = 3,         // Multiply with previous
    MUL_PREV2s = 4,        // Multiply previous two results
    MAXs = 5,              // Maximum
    MINs = 6,              // Minimum
    SETEs = 7,             // Set if equal (to zero)
    SETGTs = 8,            // Set if greater than (zero)
    SETGTEs = 9,           // Set if greater than or equal (to zero)
    SETNEs = 10,           // Set if not equal (to zero)
    FRACs = 11,            // Fractional part
    TRUNCs = 12,           // Truncate
    FLOORs = 13,           // Floor
    EXP_IEEE = 14,         // Exponential (base 2)
    LOG_CLAMP = 15,        // Log (clamped)
    LOG_IEEE = 16,         // Log (IEEE)
    RECIP_CLAMP = 17,      // Reciprocal (clamped)
    RECIP_FF = 18,         // Reciprocal (flush to zero)
    RECIP_IEEE = 19,       // Reciprocal (IEEE)
    RECIPSQ_CLAMP = 20,    // Reciprocal square root (clamped)
    RECIPSQ_FF = 21,       // Reciprocal square root (flush)
    RECIPSQ_IEEE = 22,     // Reciprocal square root (IEEE)
    MOVAs = 23,            // Move to address register
    MOVA_FLOORs = 24,      // Move floor to address register
    SUBs = 25,             // Subtract
    SUB_PREVs = 26,        // Subtract with previous
    PRED_SETEs = 27,       // Predicate set if equal
    PRED_SETNEs = 28,      // Predicate set if not equal
    PRED_SETGTs = 29,      // Predicate set if greater than
    PRED_SETGTEs = 30,     // Predicate set if greater than or equal
    PRED_SET_INVs = 31,    // Predicate set inverse
    PRED_SET_POPs = 32,    // Predicate set pop
    PRED_SET_CLRs = 33,    // Predicate set clear
    PRED_SET_RESTOREs = 34, // Predicate set restore
    KILLEs = 35,           // Kill if equal
    KILLGTs = 36,          // Kill if greater than
    KILLGTEs = 37,         // Kill if greater than or equal
    KILLNEs = 38,          // Kill if not equal
    KILLONEs = 39,         // Kill if one
    SQRT_IEEE = 40,        // Square root
    Reserved41 = 41,
    MUL_CONST_0 = 42,      // Multiply by constant 0
    MUL_CONST_1 = 43,      // Multiply by constant 1
    ADD_CONST_0 = 44,      // Add constant 0
    ADD_CONST_1 = 45,      // Add constant 1
    SUB_CONST_0 = 46,      // Subtract constant 0
    SUB_CONST_1 = 47,      // Subtract constant 1
    SIN = 48,              // Sine
    COS = 49,              // Cosine
    RETAIN_PREV = 50,      // Retain previous result
};

/**
 * Xenos ALU vector opcodes (complete list - 30 opcodes)
 */
enum class AluVectorOp : u8 {
    ADDv = 0,              // Vector add
    MULv = 1,              // Vector multiply
    MAXv = 2,              // Vector maximum
    MINv = 3,              // Vector minimum
    SETEv = 4,             // Set if equal
    SETGTv = 5,            // Set if greater than
    SETGTEv = 6,           // Set if greater than or equal
    SETNEv = 7,            // Set if not equal
    FRACv = 8,             // Fractional part
    TRUNCv = 9,            // Truncate
    FLOORv = 10,           // Floor
    MULADDv = 11,          // Multiply-add (MAD)
    CNDEv = 12,            // Conditional select if equal
    CNDGTEv = 13,          // Conditional select if greater than or equal
    CNDGTv = 14,           // Conditional select if greater than
    DOT4v = 15,            // 4-component dot product
    DOT3v = 16,            // 3-component dot product
    DOT2ADDv = 17,         // 2-component dot product + add
    CUBEv = 18,            // Cube map coordinate generation
    MAX4v = 19,            // Maximum of 4 components
    PRED_SETE_PUSHv = 20,  // Predicate set if equal, push
    PRED_SETNE_PUSHv = 21, // Predicate set if not equal, push
    PRED_SETGT_PUSHv = 22, // Predicate set if greater than, push
    PRED_SETGTE_PUSHv = 23, // Predicate set if greater than or equal, push
    KILLEv = 24,           // Kill if equal
    KILLGTv = 25,          // Kill if greater than
    KILLGTEv = 26,         // Kill if greater than or equal
    KILLNEv = 27,          // Kill if not equal
    DSTv = 28,             // Distance vector
    MOVAv = 29,            // Move to address register
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
 * Texture dimension
 */
enum class TextureDimension : u8 {
    k1D = 0,
    k2D = 1,
    k3D = 2,
    kCube = 3,
};

/**
 * Swizzle component values
 */
enum class SwizzleComponent : u8 {
    kX = 0,
    kY = 1,
    kZ = 2,
    kW = 3,
    k0 = 4,  // Constant 0
    k1 = 5,  // Constant 1
};

/**
 * Export type (where vertex shader outputs go)
 */
enum class ExportType : u8 {
    Position = 0,      // Position output (oPos)
    Interpolator = 1,  // Interpolator for pixel shader
    PointSize = 2,     // Point size output
    Color = 3,         // Color output (pixel shader)
    Depth = 4,         // Depth output (pixel shader)
};

/**
 * Control flow opcodes
 */
enum class ControlFlowOp : u8 {
    NOP = 0,
    EXEC = 1,
    EXEC_END = 2,
    COND_EXEC = 3,
    COND_EXEC_END = 4,
    COND_PRED_EXEC = 5,
    COND_PRED_EXEC_END = 6,
    LOOP_START = 7,
    LOOP_END = 8,
    COND_CALL = 9,
    RETURN = 10,
    COND_JMP = 11,
    ALLOC = 12,
    COND_EXEC_PRED_CLEAN = 13,
    COND_EXEC_PRED_CLEAN_END = 14,
    MARK_VS_FETCH_DONE = 15,
};

/**
 * Allocation type (for ALLOC instruction)
 */
enum class AllocType : u8 {
    None = 0,
    Position = 1,
    Interpolators = 2,
    Pixel = 3,
};

//=============================================================================
// SPIR-V Builder
//=============================================================================

/**
 * SPIR-V builder for shader translation
 * Generates SPIR-V binary from shader operations
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
    
    // === Type declarations ===
    u32 type_void();
    u32 type_bool();
    u32 type_int(u32 width, bool signed_);
    u32 type_float(u32 width);
    u32 type_vector(u32 component_type, u32 count);
    u32 type_matrix(u32 column_type, u32 columns);
    u32 type_array(u32 element_type, u32 length);
    u32 type_runtime_array(u32 element_type);
    u32 type_struct(const std::vector<u32>& members);
    u32 type_pointer(u32 storage_class, u32 type);
    u32 type_function(u32 return_type, const std::vector<u32>& params);
    u32 type_image(u32 sampled_type, u32 dim, bool depth, bool arrayed, bool ms, u32 sampled);
    u32 type_sampled_image(u32 image_type);
    u32 type_sampler();
    
    // === Constants ===
    u32 const_bool(bool value);
    u32 const_int(s32 value);
    u32 const_uint(u32 value);
    u32 const_float(f32 value);
    u32 const_composite(u32 type, const std::vector<u32>& constituents);
    
    // === Variables ===
    u32 variable(u32 pointer_type, u32 storage_class, u32 initializer = 0);
    
    // === Function control ===
    void function_begin(u32 return_type, u32 function_type);
    void function_end();
    void label(u32 id);
    void return_void();
    void return_value(u32 value);
    
    // === Memory operations ===
    u32 load(u32 result_type, u32 pointer);
    void store(u32 pointer, u32 value);
    u32 access_chain(u32 result_type, u32 base, const std::vector<u32>& indices);
    
    // === Arithmetic ===
    u32 f_add(u32 type, u32 a, u32 b);
    u32 f_sub(u32 type, u32 a, u32 b);
    u32 f_mul(u32 type, u32 a, u32 b);
    u32 f_div(u32 type, u32 a, u32 b);
    u32 f_negate(u32 type, u32 a);
    u32 f_mod(u32 type, u32 a, u32 b);
    u32 i_add(u32 type, u32 a, u32 b);
    u32 i_sub(u32 type, u32 a, u32 b);
    u32 i_mul(u32 type, u32 a, u32 b);
    
    // === Conversions ===
    u32 convert_f_to_s(u32 type, u32 value);
    u32 convert_s_to_f(u32 type, u32 value);
    u32 convert_f_to_u(u32 type, u32 value);
    u32 convert_u_to_f(u32 type, u32 value);
    u32 bitcast(u32 type, u32 value);
    
    // === Math functions via GLSL.std.450 ===
    u32 ext_inst(u32 type, u32 set, u32 instruction, const std::vector<u32>& operands);
    
    // === Vector operations ===
    u32 vector_shuffle(u32 type, u32 v1, u32 v2, const std::vector<u32>& components);
    u32 composite_extract(u32 type, u32 composite, const std::vector<u32>& indices);
    u32 composite_insert(u32 type, u32 object, u32 composite, const std::vector<u32>& indices);
    u32 composite_construct(u32 type, const std::vector<u32>& constituents);
    u32 vector_extract_dynamic(u32 type, u32 vector, u32 index);
    u32 vector_insert_dynamic(u32 type, u32 vector, u32 component, u32 index);
    
    // === Dot product (native SPIR-V op) ===
    u32 dot(u32 type, u32 a, u32 b);
    
    // === Comparison ===
    u32 f_ord_equal(u32 type, u32 a, u32 b);
    u32 f_ord_not_equal(u32 type, u32 a, u32 b);
    u32 f_ord_less_than(u32 type, u32 a, u32 b);
    u32 f_ord_greater_than(u32 type, u32 a, u32 b);
    u32 f_ord_less_than_equal(u32 type, u32 a, u32 b);
    u32 f_ord_greater_than_equal(u32 type, u32 a, u32 b);
    u32 i_equal(u32 type, u32 a, u32 b);
    u32 i_not_equal(u32 type, u32 a, u32 b);
    
    // === Logical ===
    u32 logical_and(u32 type, u32 a, u32 b);
    u32 logical_or(u32 type, u32 a, u32 b);
    u32 logical_not(u32 type, u32 a);
    u32 any(u32 type, u32 vector);
    u32 all(u32 type, u32 vector);
    
    // === Control flow ===
    u32 select(u32 type, u32 condition, u32 true_val, u32 false_val);
    void branch(u32 target);
    void branch_conditional(u32 condition, u32 true_label, u32 false_label);
    void loop_merge(u32 merge_block, u32 continue_target, u32 control);
    void selection_merge(u32 merge_block, u32 control);
    void kill();  // OpKill for fragment shaders
    
    // === Phi node for SSA ===
    u32 phi(u32 type, const std::vector<std::pair<u32, u32>>& incoming);
    
    // === Texture operations ===
    u32 sampled_image(u32 type, u32 image, u32 sampler);
    u32 image_sample(u32 type, u32 sampled_image, u32 coord, u32 bias = 0);
    u32 image_sample_lod(u32 type, u32 sampled_image, u32 coord, u32 lod);
    u32 image_sample_grad(u32 type, u32 sampled_image, u32 coord, u32 ddx, u32 ddy);
    u32 image_fetch(u32 type, u32 image, u32 coord, u32 lod = 0);
    u32 image_query_size_lod(u32 type, u32 image, u32 lod);
    
    // === Decorations ===
    void decorate(u32 target, u32 decoration, const std::vector<u32>& operands = {});
    void member_decorate(u32 type, u32 member, u32 decoration, const std::vector<u32>& operands = {});
    void decorate_array_stride(u32 type, u32 stride);
    
    // === Debug ===
    void name(u32 target, const std::string& name);
    void member_name(u32 type, u32 member, const std::string& name);
    
    // === Entry point ===
    void entry_point(u32 execution_model, u32 entry_point, const std::string& name, 
                    const std::vector<u32>& interface);
    void execution_mode(u32 entry_point, u32 mode, const std::vector<u32>& operands = {});
    
    // === Extensions ===
    u32 import_extension(const std::string& name);
    void capability(u32 cap);
    void memory_model(u32 addressing, u32 memory);
    
    // === ID management ===
    u32 allocate_id() { return next_id_++; }
    u32 reserve_id() { return next_id_++; }
    
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
    
    // Constant cache
    std::unordered_map<u64, u32> const_cache_;
    
    // Emit helpers
    void emit(std::vector<u32>& target, u32 word);
    void emit_op(std::vector<u32>& target, u32 opcode, u32 result_type, u32 result_id, 
                const std::vector<u32>& operands);
};

//=============================================================================
// Shader Microcode Parser
//=============================================================================

/**
 * Xenos shader microcode parser
 * Decodes the binary microcode format into structured instructions
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
    u32 instruction_count() const { return static_cast<u32>(instructions_.size()); }
    
    /**
     * ALU instruction encoding (96 bits = 3 dwords)
     */
    struct AluInstruction {
        u32 words[3];
        
        // Vector operation fields
        u8 vector_opcode;
        u8 vector_dest;
        u8 vector_dest_rel;
        u8 vector_write_mask;
        bool vector_saturate;
        
        // Scalar operation fields
        u8 scalar_opcode;
        u8 scalar_dest;
        u8 scalar_dest_rel;
        u8 scalar_write_mask;
        bool scalar_saturate;
        
        // Source registers
        u8 src_regs[3];
        u8 src_swizzles[3];
        bool src_abs[3];
        bool src_negate[3];
        bool src_is_const[3];
        
        // Combined fields
        u8 dest_reg;
        u8 write_mask;
        bool abs[3];
        bool negate[3];
        
        // Export fields
        bool export_data;
        u8 export_type;
        u8 export_index;
        
        // Predicate
        bool predicated;
        bool predicate_condition;
    };
    
    /**
     * Fetch instruction encoding (96 bits = 3 dwords)
     */
    struct FetchInstruction {
        u32 words[3];
        
        // Common fields
        u8 opcode;
        u8 dest_reg;
        u8 dest_swizzle;
        u8 src_reg;
        u8 src_swizzle;
        
        // Vertex fetch fields
        u8 const_index;
        u8 fetch_type;
        u32 offset;
        u8 data_format;
        bool signed_rf;
        u8 num_format;
        u16 stride;
        
        // Texture fetch fields
        TextureDimension dimension;
        bool use_computed_lod;
        bool use_register_lod;
        bool use_register_gradients;
        f32 lod_bias;
        u8 offset_x, offset_y, offset_z;
        
        // Predicate
        bool predicated;
        bool predicate_condition;
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
        bool is_fetch;
        u8 alloc_type;
        u8 alloc_count;
        
        // Loop fields
        u8 loop_id;
    };
    
    // Access decoded instructions
    const std::vector<ControlFlowInstruction>& cf_instructions() const { return cf_instructions_; }
    const std::vector<AluInstruction>& alu_instructions() const { return alu_instructions_; }
    const std::vector<FetchInstruction>& fetch_instructions() const { return fetch_instructions_; }
    
    // Raw instructions access
    const std::vector<u32>& raw_instructions() const { return instructions_; }
    
private:
    ShaderType type_;
    std::vector<ControlFlowInstruction> cf_instructions_;
    std::vector<AluInstruction> alu_instructions_;
    std::vector<FetchInstruction> fetch_instructions_;
    std::vector<u32> instructions_;
    
    void decode_control_flow();
    void decode_alu_clause(u32 address, u32 count);
    void decode_fetch_clause(u32 address, u32 count);
    void decode_alu_instruction(AluInstruction& inst);
    void decode_fetch_instruction(FetchInstruction& inst);
};

//=============================================================================
// Shader Analysis
//=============================================================================

/**
 * Analyzed shader information
 */
struct ShaderInfo {
    ShaderType type;
    u32 instruction_count;
    
    // Register usage analysis
    u32 temp_register_count;
    u32 max_const_register;
    u32 bool_const_count;
    u32 loop_const_count;
    
    // Texture bindings used
    std::vector<u32> texture_bindings;
    std::vector<TextureDimension> texture_dimensions;
    
    // Vertex fetch slots used
    std::vector<u32> vertex_fetch_slots;
    
    // Interpolants (vertex outputs / pixel inputs)
    struct Interpolant {
        u32 index;
        u32 component_count;
        bool flat;
        bool perspective;
    };
    std::vector<Interpolant> interpolants;
    
    // Export info
    bool exports_position;
    bool exports_point_size;
    u32 color_export_count;
    bool exports_depth;
    
    // Control flow complexity
    u32 loop_count;
    u32 conditional_count;
    bool uses_predication;
    bool uses_kill;
};

//=============================================================================
// Shader Translator
//=============================================================================

/**
 * Main shader translator class
 * Converts Xenos shader microcode to SPIR-V
 */
class ShaderTranslator {
public:
    ShaderTranslator();
    ~ShaderTranslator();
    
    /**
     * Initialize with cache path
     */
    Status initialize(const std::string& cache_path);
    
    /**
     * Shutdown and save cache
     */
    void shutdown();
    
    /**
     * Translate Xenos shader to SPIR-V
     * @param microcode Pointer to shader microcode
     * @param size Size in bytes
     * @param type Vertex or pixel shader
     * @return SPIR-V binary (empty on failure)
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
    
    /**
     * Clear cache
     */
    void clear_cache() { cache_.clear(); }
    
    /**
     * Get statistics
     */
    struct Stats {
        u64 shaders_translated;
        u64 cache_hits;
        u64 cache_misses;
        u64 total_spirv_size;
        u64 total_microcode_size;
    };
    Stats get_stats() const { return stats_; }
    
    /**
     * Analyze shader without translating
     */
    ShaderInfo analyze(const void* microcode, u32 size, ShaderType type);
    
private:
    std::string cache_path_;
    std::unordered_map<u64, std::vector<u32>> cache_;
    Stats stats_{};
    
    // Translation context - holds all state during translation
    struct TranslationContext {
        SpirvBuilder builder;
        ShaderType type;
        const ShaderMicrocode* microcode;
        ShaderInfo info;
        
        // Type IDs (cached)
        u32 void_type;
        u32 bool_type;
        u32 int_type;
        u32 uint_type;
        u32 float_type;
        u32 vec2_type;
        u32 vec3_type;
        u32 vec4_type;
        u32 ivec2_type;
        u32 ivec3_type;
        u32 ivec4_type;
        u32 uvec4_type;
        u32 mat4_type;
        u32 bvec4_type;
        
        // Built-in inputs/outputs
        u32 position_var;          // gl_Position (vertex output)
        u32 point_size_var;        // gl_PointSize (vertex output)
        u32 vertex_id_var;         // gl_VertexIndex (vertex input)
        u32 instance_id_var;       // gl_InstanceIndex (vertex input)
        u32 frag_coord_var;        // gl_FragCoord (fragment input)
        u32 front_facing_var;      // gl_FrontFacing (fragment input)
        u32 frag_depth_var;        // gl_FragDepth (fragment output)
        u32 frag_color_var;        // Main fragment color output
        
        // Fragment outputs (MRT)
        std::array<u32, 4> color_outputs;
        
        // Uniform buffers
        u32 vertex_constants_var;   // Vertex shader float constants
        u32 pixel_constants_var;    // Pixel shader float constants
        u32 bool_constants_var;     // Boolean constants
        u32 loop_constants_var;     // Loop constants
        
        // Texture samplers (up to 16)
        std::array<u32, 16> sampler_vars;
        std::array<u32, 16> texture_vars;
        std::array<u32, 16> sampled_image_types;
        
        // Vertex inputs (up to 16 attributes)
        std::array<u32, 16> vertex_input_vars;
        
        // Interpolants (vertex output / fragment input)
        std::array<u32, 16> interpolant_vars;
        
        // Temporaries (128 available in Xenos)
        std::array<u32, 128> temp_vars;
        
        // Address register (a0)
        u32 address_reg_var;
        
        // Predicate register
        u32 predicate_var;
        
        // Previous scalar result (for chained ops)
        u32 prev_scalar;
        u32 prev_vector;
        
        // Loop counter stack
        std::vector<u32> loop_counter_vars;
        
        // GLSL.std.450 extension ID
        u32 glsl_ext;
        
        // Entry point function ID
        u32 main_function;
        
        // Current loop labels (for break/continue)
        struct LoopInfo {
            u32 header_label;
            u32 continue_label;
            u32 merge_label;
            u32 counter_var;
        };
        std::vector<LoopInfo> loop_stack;
        
        // Constant values (cached for reuse)
        u32 const_zero;
        u32 const_one;
        u32 const_neg_one;
        u32 const_half;
        u32 const_two;
        u32 const_vec4_zero;
        u32 const_vec4_one;
        
        // Pointer types (cached)
        u32 float_func_ptr;
        u32 vec4_func_ptr;
        u32 vec4_uniform_ptr;
        u32 bool_func_ptr;
        u32 int_func_ptr;
    };
    
    // Setup functions
    void setup_types(TranslationContext& ctx);
    void setup_constants(TranslationContext& ctx);
    void setup_inputs(TranslationContext& ctx);
    void setup_outputs(TranslationContext& ctx);
    void setup_uniforms(TranslationContext& ctx);
    void setup_samplers(TranslationContext& ctx, const ShaderInfo& info);
    void setup_temporaries(TranslationContext& ctx);
    void setup_interpolants(TranslationContext& ctx, const ShaderInfo& info);
    
    // Main translation
    void translate_shader_body(TranslationContext& ctx, const ShaderMicrocode& microcode);
    void translate_control_flow(TranslationContext& ctx, const ShaderMicrocode& microcode);
    void translate_exec_clause(TranslationContext& ctx, const ShaderMicrocode::ControlFlowInstruction& cf,
                              const ShaderMicrocode& microcode);
    void translate_alu_instruction(TranslationContext& ctx, const ShaderMicrocode::AluInstruction& inst);
    void translate_fetch_instruction(TranslationContext& ctx, const ShaderMicrocode::FetchInstruction& inst);
    
    // ALU operation translation
    u32 translate_scalar_op(TranslationContext& ctx, AluScalarOp op, u32 src, u32 type);
    u32 translate_vector_op(TranslationContext& ctx, AluVectorOp op,
                           const std::vector<u32>& sources, u32 type);
    
    // Source operand handling
    u32 get_source_register(TranslationContext& ctx, u8 reg, bool abs, bool negate);
    u32 get_source_with_swizzle(TranslationContext& ctx, u8 reg, u8 swizzle, bool abs, bool negate, bool is_const);
    u32 apply_swizzle(TranslationContext& ctx, u32 value, u8 swizzle);
    u32 apply_source_modifier(TranslationContext& ctx, u32 value, bool abs, bool negate);
    
    // Destination handling
    void write_dest_register(TranslationContext& ctx, u8 reg, u32 value, u8 write_mask);
    void write_dest_with_saturate(TranslationContext& ctx, u8 reg, u32 value, u8 write_mask, bool saturate);
    u32 apply_write_mask(TranslationContext& ctx, u32 dest_ptr, u32 new_value, u8 mask);
    u32 apply_saturate(TranslationContext& ctx, u32 value);
    
    // Export handling
    void handle_export(TranslationContext& ctx, u32 value, u8 export_type, u8 export_index);
    void write_position(TranslationContext& ctx, u32 value);
    void write_interpolant(TranslationContext& ctx, u32 value, u8 index);
    void write_color(TranslationContext& ctx, u32 value, u8 index);
    
    // Texture operations
    u32 translate_texture_fetch(TranslationContext& ctx, const ShaderMicrocode::FetchInstruction& inst);
    u32 translate_vertex_fetch(TranslationContext& ctx, const ShaderMicrocode::FetchInstruction& inst);
    u32 get_texture_coord(TranslationContext& ctx, u8 src_reg, u8 swizzle, TextureDimension dim);
    
    // Control flow
    void translate_loop(TranslationContext& ctx, const ShaderMicrocode::ControlFlowInstruction& start,
                       const ShaderMicrocode& microcode);
    void translate_conditional(TranslationContext& ctx, const ShaderMicrocode::ControlFlowInstruction& cf,
                              const ShaderMicrocode& microcode);
    void emit_kill(TranslationContext& ctx, u32 condition);
    
    // Cube map helper
    u32 emit_cube_vector(TranslationContext& ctx, u32 src0, u32 src1);
    
    // Utility
    static u64 compute_hash(const void* data, u32 size);
    ShaderInfo analyze_shader(const ShaderMicrocode& microcode);
    
    // Helper to get constant register value  
    u32 get_constant(TranslationContext& ctx, u32 index);
    u32 get_bool_constant(TranslationContext& ctx, u32 index);
    u32 get_loop_constant(TranslationContext& ctx, u32 index);
};

} // namespace x360mu
