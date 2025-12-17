/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos Shader Translator
 * 
 * Converts Xbox 360 Xenos GPU shader microcode to SPIR-V for Vulkan rendering.
 * The Xenos uses a custom shader ISA that must be translated to modern APIs.
 */

#include "shader_translator.h"
#include <cstring>
#include <cmath>

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
// SPIR-V Constants
//=============================================================================

namespace spirv {
    // Magic number
    constexpr u32 MAGIC = 0x07230203;
    constexpr u32 VERSION = 0x00010000; // SPIR-V 1.0
    
    // Opcodes
    constexpr u32 OpCapability = 17;
    constexpr u32 OpExtInstImport = 11;
    constexpr u32 OpMemoryModel = 14;
    constexpr u32 OpEntryPoint = 15;
    constexpr u32 OpExecutionMode = 16;
    constexpr u32 OpName = 5;
    constexpr u32 OpMemberName = 6;
    constexpr u32 OpDecorate = 71;
    constexpr u32 OpMemberDecorate = 72;
    constexpr u32 OpTypeVoid = 19;
    constexpr u32 OpTypeBool = 20;
    constexpr u32 OpTypeInt = 21;
    constexpr u32 OpTypeFloat = 22;
    constexpr u32 OpTypeVector = 23;
    constexpr u32 OpTypeMatrix = 24;
    constexpr u32 OpTypeImage = 25;
    constexpr u32 OpTypeSampledImage = 27;
    constexpr u32 OpTypeArray = 28;
    constexpr u32 OpTypeStruct = 30;
    constexpr u32 OpTypePointer = 32;
    constexpr u32 OpTypeFunction = 33;
    constexpr u32 OpConstant = 43;
    constexpr u32 OpConstantComposite = 44;
    constexpr u32 OpVariable = 59;
    constexpr u32 OpLoad = 61;
    constexpr u32 OpStore = 62;
    constexpr u32 OpAccessChain = 65;
    constexpr u32 OpFunction = 54;
    constexpr u32 OpFunctionEnd = 56;
    constexpr u32 OpLabel = 248;
    constexpr u32 OpReturn = 253;
    constexpr u32 OpReturnValue = 254;
    constexpr u32 OpFAdd = 129;
    constexpr u32 OpFSub = 131;
    constexpr u32 OpFMul = 133;
    constexpr u32 OpFDiv = 136;
    constexpr u32 OpFNegate = 127;
    constexpr u32 OpFMod = 141;
    constexpr u32 OpVectorShuffle = 79;
    constexpr u32 OpCompositeExtract = 81;
    constexpr u32 OpCompositeConstruct = 80;
    constexpr u32 OpDot = 148;
    constexpr u32 OpImageSampleImplicitLod = 87;
    constexpr u32 OpSelect = 169;
    constexpr u32 OpFOrdEqual = 180;
    constexpr u32 OpFOrdNotEqual = 182;
    constexpr u32 OpFOrdLessThan = 184;
    constexpr u32 OpFOrdGreaterThan = 186;
    constexpr u32 OpFOrdLessThanEqual = 188;
    constexpr u32 OpFOrdGreaterThanEqual = 190;
    constexpr u32 OpBranch = 249;
    constexpr u32 OpBranchConditional = 250;
    constexpr u32 OpKill = 252;
    
    // Capabilities
    constexpr u32 CapabilityShader = 1;
    constexpr u32 CapabilitySampled1D = 43;
    
    // Storage classes
    constexpr u32 StorageClassInput = 1;
    constexpr u32 StorageClassOutput = 3;
    constexpr u32 StorageClassUniform = 2;
    constexpr u32 StorageClassUniformConstant = 0;
    constexpr u32 StorageClassFunction = 7;
    
    // Decorations
    constexpr u32 DecorationLocation = 30;
    constexpr u32 DecorationBinding = 33;
    constexpr u32 DecorationDescriptorSet = 34;
    constexpr u32 DecorationBuiltIn = 11;
    constexpr u32 DecorationBlock = 2;
    constexpr u32 DecorationOffset = 35;
    
    // Built-ins
    constexpr u32 BuiltInPosition = 0;
    constexpr u32 BuiltInVertexIndex = 42;
    constexpr u32 BuiltInFragCoord = 15;
    constexpr u32 BuiltInFrontFacing = 17;
    
    // Execution models
    constexpr u32 ExecutionModelVertex = 0;
    constexpr u32 ExecutionModelFragment = 4;
    
    // Execution modes
    constexpr u32 ExecutionModeOriginUpperLeft = 7;
    
    // GLSL.std.450 extended instructions
    constexpr u32 GLSLstd450Round = 1;
    constexpr u32 GLSLstd450RoundEven = 2;
    constexpr u32 GLSLstd450Trunc = 3;
    constexpr u32 GLSLstd450FAbs = 4;
    constexpr u32 GLSLstd450Floor = 8;
    constexpr u32 GLSLstd450Ceil = 9;
    constexpr u32 GLSLstd450Fract = 10;
    constexpr u32 GLSLstd450Sin = 13;
    constexpr u32 GLSLstd450Cos = 14;
    constexpr u32 GLSLstd450Exp2 = 29;
    constexpr u32 GLSLstd450Log2 = 30;
    constexpr u32 GLSLstd450Sqrt = 31;
    constexpr u32 GLSLstd450InverseSqrt = 32;
    constexpr u32 GLSLstd450FMin = 37;
    constexpr u32 GLSLstd450FMax = 40;
    constexpr u32 GLSLstd450FClamp = 43;
    constexpr u32 GLSLstd450FMix = 46;
    constexpr u32 GLSLstd450Normalize = 69;
    constexpr u32 GLSLstd450Reflect = 71;
}

//=============================================================================
// Xenos Shader Microcode Parsing
//=============================================================================

struct XenosAluInstruction {
    // Decoded ALU instruction fields
    AluScalarOp scalar_op;
    AluVectorOp vector_op;
    
    u8 scalar_dest;
    u8 vector_dest;
    u8 scalar_write_mask;
    u8 vector_write_mask;
    
    // Source registers
    struct Source {
        u8 reg;
        u8 swizzle;
        bool negate;
        bool absolute;
        bool is_const;
    };
    
    Source src1, src2, src3;
    
    bool pred_invert;
    u8 pred_sel;
    bool export_data;
    u8 export_reg;
};

struct XenosFetchInstruction {
    FetchOp op;
    u8 dest_reg;
    u8 dest_swizzle;
    u8 src_reg;
    u8 const_index;
    bool is_mini_fetch;
    
    // Texture fetch specific
    u8 tex_coord_swizzle;
    bool fetch_valid_only;
    bool unnormalized_coords;
    s8 lod_bias;
    bool use_computed_lod;
};

static XenosAluInstruction decode_alu_instruction(const u32* words) {
    XenosAluInstruction inst = {};
    
    // Xenos ALU instructions are 3 dwords (96 bits)
    u32 w0 = words[0];
    u32 w1 = words[1];
    u32 w2 = words[2];
    
    // Extract scalar operation (bits 0-5 of word 2)
    inst.scalar_op = static_cast<AluScalarOp>(w2 & 0x3F);
    
    // Extract vector operation (bits 6-10 of word 2)
    inst.vector_op = static_cast<AluVectorOp>((w2 >> 6) & 0x1F);
    
    // Extract destinations
    inst.scalar_dest = (w2 >> 11) & 0x3F;
    inst.vector_dest = (w2 >> 17) & 0x3F;
    inst.scalar_write_mask = (w2 >> 23) & 0xF;
    inst.vector_write_mask = (w2 >> 27) & 0xF;
    
    // Source 1
    inst.src1.reg = w0 & 0x3F;
    inst.src1.swizzle = (w0 >> 6) & 0xFF;
    inst.src1.negate = (w0 >> 14) & 1;
    inst.src1.absolute = (w0 >> 15) & 1;
    inst.src1.is_const = (w0 >> 16) & 1;
    
    // Source 2
    inst.src2.reg = (w0 >> 17) & 0x3F;
    inst.src2.swizzle = (w0 >> 23) & 0xFF;
    inst.src2.negate = (w1 >> 0) & 1;
    inst.src2.absolute = (w1 >> 1) & 1;
    inst.src2.is_const = (w1 >> 2) & 1;
    
    // Source 3
    inst.src3.reg = (w1 >> 3) & 0x3F;
    inst.src3.swizzle = (w1 >> 9) & 0xFF;
    inst.src3.negate = (w1 >> 17) & 1;
    inst.src3.absolute = (w1 >> 18) & 1;
    inst.src3.is_const = (w1 >> 19) & 1;
    
    // Predicate
    inst.pred_invert = (w1 >> 20) & 1;
    inst.pred_sel = (w1 >> 21) & 3;
    
    // Export
    inst.export_data = (w1 >> 23) & 1;
    inst.export_reg = (w1 >> 24) & 0xFF;
    
    return inst;
}

static XenosFetchInstruction decode_fetch_instruction(const u32* words) {
    XenosFetchInstruction inst = {};
    
    // Fetch instructions are 3 dwords
    u32 w0 = words[0];
    u32 w1 = words[1];
    u32 w2 = words[2];
    
    inst.op = static_cast<FetchOp>((w0 >> 0) & 0x1F);
    inst.dest_reg = (w0 >> 5) & 0x3F;
    inst.dest_swizzle = (w0 >> 11) & 0xFF;
    inst.src_reg = (w0 >> 19) & 0x3F;
    inst.const_index = (w0 >> 25) & 0x1F;
    
    inst.is_mini_fetch = (w1 >> 0) & 1;
    inst.tex_coord_swizzle = (w1 >> 1) & 0xFF;
    inst.fetch_valid_only = (w1 >> 9) & 1;
    inst.unnormalized_coords = (w1 >> 10) & 1;
    inst.lod_bias = (s8)((w1 >> 11) & 0x7F);
    inst.use_computed_lod = (w1 >> 18) & 1;
    
    return inst;
}

//=============================================================================
// SPIR-V Builder Implementation
//=============================================================================

SpirvBuilder::SpirvBuilder() = default;

void SpirvBuilder::begin(ShaderType type) {
    next_id_ = 1;
    capabilities_.clear();
    extensions_.clear();
    ext_inst_imports_.clear();
    memory_model_.clear();
    entry_points_.clear();
    execution_modes_.clear();
    debug_names_.clear();
    decorations_.clear();
    types_constants_.clear();
    globals_.clear();
    functions_.clear();
    current_function_.clear();
    type_cache_.clear();
    
    // Add Shader capability
    capability(spirv::CapabilityShader);
}

std::vector<u32> SpirvBuilder::end() {
    std::vector<u32> result;
    
    // Header
    result.push_back(spirv::MAGIC);
    result.push_back(spirv::VERSION);
    result.push_back(0); // Generator (0 = unknown)
    result.push_back(next_id_); // Bound
    result.push_back(0); // Schema
    
    // Sections in order
    result.insert(result.end(), capabilities_.begin(), capabilities_.end());
    result.insert(result.end(), extensions_.begin(), extensions_.end());
    result.insert(result.end(), ext_inst_imports_.begin(), ext_inst_imports_.end());
    result.insert(result.end(), memory_model_.begin(), memory_model_.end());
    result.insert(result.end(), entry_points_.begin(), entry_points_.end());
    result.insert(result.end(), execution_modes_.begin(), execution_modes_.end());
    result.insert(result.end(), debug_names_.begin(), debug_names_.end());
    result.insert(result.end(), decorations_.begin(), decorations_.end());
    result.insert(result.end(), types_constants_.begin(), types_constants_.end());
    result.insert(result.end(), globals_.begin(), globals_.end());
    result.insert(result.end(), functions_.begin(), functions_.end());
    
    return result;
}

void SpirvBuilder::emit(std::vector<u32>& target, u32 word) {
    target.push_back(word);
}

void SpirvBuilder::emit_op(std::vector<u32>& target, u32 opcode, u32 result_type, 
                           u32 result_id, const std::vector<u32>& operands) {
    u32 word_count = 1 + (result_type ? 1 : 0) + (result_id ? 1 : 0) + operands.size();
    emit(target, (word_count << 16) | opcode);
    if (result_type) emit(target, result_type);
    if (result_id) emit(target, result_id);
    for (u32 op : operands) emit(target, op);
}

u32 SpirvBuilder::type_void() {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeVoid, 0, id, {});
    return id;
}

u32 SpirvBuilder::type_bool() {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeBool, 0, id, {});
    return id;
}

u32 SpirvBuilder::type_int(u32 width, bool signed_) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeInt, 0, id, {width, signed_ ? 1u : 0u});
    return id;
}

u32 SpirvBuilder::type_float(u32 width) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeFloat, 0, id, {width});
    return id;
}

u32 SpirvBuilder::type_vector(u32 component_type, u32 count) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeVector, 0, id, {component_type, count});
    return id;
}

u32 SpirvBuilder::type_matrix(u32 column_type, u32 columns) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeMatrix, 0, id, {column_type, columns});
    return id;
}

u32 SpirvBuilder::type_array(u32 element_type, u32 length) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeArray, 0, id, {element_type, length});
    return id;
}

u32 SpirvBuilder::type_struct(const std::vector<u32>& members) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeStruct, 0, id, members);
    return id;
}

u32 SpirvBuilder::type_pointer(u32 storage_class, u32 type) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypePointer, 0, id, {storage_class, type});
    return id;
}

u32 SpirvBuilder::type_function(u32 return_type, const std::vector<u32>& params) {
    u32 id = allocate_id();
    std::vector<u32> ops = {return_type};
    ops.insert(ops.end(), params.begin(), params.end());
    emit_op(types_constants_, spirv::OpTypeFunction, 0, id, ops);
    return id;
}

u32 SpirvBuilder::type_sampled_image(u32 image_type) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpTypeSampledImage, 0, id, {image_type});
    return id;
}

u32 SpirvBuilder::const_bool(bool value) {
    u32 id = allocate_id();
    u32 bool_type = type_bool();
    emit_op(types_constants_, value ? 41 : 42, bool_type, id, {}); // OpConstantTrue/False
    return id;
}

u32 SpirvBuilder::const_int(s32 value) {
    u32 id = allocate_id();
    u32 int_type = type_int(32, true);
    emit_op(types_constants_, spirv::OpConstant, int_type, id, {static_cast<u32>(value)});
    return id;
}

u32 SpirvBuilder::const_uint(u32 value) {
    u32 id = allocate_id();
    u32 uint_type = type_int(32, false);
    emit_op(types_constants_, spirv::OpConstant, uint_type, id, {value});
    return id;
}

u32 SpirvBuilder::const_float(f32 value) {
    u32 id = allocate_id();
    u32 float_type = type_float(32);
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    emit_op(types_constants_, spirv::OpConstant, float_type, id, {bits});
    return id;
}

u32 SpirvBuilder::const_composite(u32 type, const std::vector<u32>& constituents) {
    u32 id = allocate_id();
    emit_op(types_constants_, spirv::OpConstantComposite, type, id, constituents);
    return id;
}

u32 SpirvBuilder::variable(u32 pointer_type, u32 storage_class, u32 initializer) {
    u32 id = allocate_id();
    std::vector<u32> ops = {storage_class};
    if (initializer) ops.push_back(initializer);
    if (storage_class == spirv::StorageClassFunction) {
        emit_op(current_function_, spirv::OpVariable, pointer_type, id, ops);
    } else {
        emit_op(globals_, spirv::OpVariable, pointer_type, id, ops);
    }
    return id;
}

void SpirvBuilder::function_begin(u32 return_type, u32 function_type) {
    u32 id = allocate_id();
    emit_op(functions_, spirv::OpFunction, return_type, id, {0, function_type}); // FunctionControlNone
}

void SpirvBuilder::function_end() {
    functions_.insert(functions_.end(), current_function_.begin(), current_function_.end());
    current_function_.clear();
    emit_op(functions_, spirv::OpFunctionEnd, 0, 0, {});
}

void SpirvBuilder::label(u32 id) {
    emit_op(current_function_, spirv::OpLabel, 0, id, {});
}

void SpirvBuilder::return_void() {
    emit_op(current_function_, spirv::OpReturn, 0, 0, {});
}

void SpirvBuilder::return_value(u32 value) {
    emit_op(current_function_, spirv::OpReturnValue, 0, 0, {value});
}

u32 SpirvBuilder::load(u32 result_type, u32 pointer) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpLoad, result_type, id, {pointer});
    return id;
}

void SpirvBuilder::store(u32 pointer, u32 value) {
    emit_op(current_function_, spirv::OpStore, 0, 0, {pointer, value});
}

u32 SpirvBuilder::access_chain(u32 result_type, u32 base, const std::vector<u32>& indices) {
    u32 id = allocate_id();
    std::vector<u32> ops = {base};
    ops.insert(ops.end(), indices.begin(), indices.end());
    emit_op(current_function_, spirv::OpAccessChain, result_type, id, ops);
    return id;
}

u32 SpirvBuilder::f_add(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpFAdd, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_sub(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpFSub, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_mul(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpFMul, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_div(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpFDiv, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_negate(u32 type, u32 a) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpFNegate, type, id, {a});
    return id;
}

u32 SpirvBuilder::ext_inst(u32 type, u32 set, u32 instruction, const std::vector<u32>& operands) {
    u32 id = allocate_id();
    std::vector<u32> ops = {set, instruction};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(current_function_, 12, type, id, ops); // OpExtInst
    return id;
}

u32 SpirvBuilder::vector_shuffle(u32 type, u32 v1, u32 v2, const std::vector<u32>& components) {
    u32 id = allocate_id();
    std::vector<u32> ops = {v1, v2};
    ops.insert(ops.end(), components.begin(), components.end());
    emit_op(current_function_, spirv::OpVectorShuffle, type, id, ops);
    return id;
}

u32 SpirvBuilder::composite_extract(u32 type, u32 composite, const std::vector<u32>& indices) {
    u32 id = allocate_id();
    std::vector<u32> ops = {composite};
    ops.insert(ops.end(), indices.begin(), indices.end());
    emit_op(current_function_, spirv::OpCompositeExtract, type, id, ops);
    return id;
}

u32 SpirvBuilder::composite_construct(u32 type, const std::vector<u32>& constituents) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpCompositeConstruct, type, id, constituents);
    return id;
}

u32 SpirvBuilder::select(u32 type, u32 condition, u32 true_val, u32 false_val) {
    u32 id = allocate_id();
    emit_op(current_function_, spirv::OpSelect, type, id, {condition, true_val, false_val});
    return id;
}

u32 SpirvBuilder::image_sample(u32 type, u32 sampled_image, u32 coord, u32 bias) {
    u32 id = allocate_id();
    if (bias) {
        emit_op(current_function_, spirv::OpImageSampleImplicitLod, type, id, 
                {sampled_image, coord, 1, bias}); // Bias = 1
    } else {
        emit_op(current_function_, spirv::OpImageSampleImplicitLod, type, id, 
                {sampled_image, coord});
    }
    return id;
}

void SpirvBuilder::decorate(u32 target, u32 decoration, const std::vector<u32>& operands) {
    std::vector<u32> ops = {target, decoration};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(decorations_, spirv::OpDecorate, 0, 0, ops);
}

void SpirvBuilder::member_decorate(u32 type, u32 member, u32 decoration, 
                                   const std::vector<u32>& operands) {
    std::vector<u32> ops = {type, member, decoration};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(decorations_, spirv::OpMemberDecorate, 0, 0, ops);
}

void SpirvBuilder::name(u32 target, const std::string& str) {
    std::vector<u32> ops = {target};
    // Pack string into words
    size_t len = str.length() + 1;
    size_t words = (len + 3) / 4;
    for (size_t i = 0; i < words; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && i * 4 + j < len; j++) {
            word |= static_cast<u32>(str[i * 4 + j]) << (j * 8);
        }
        ops.push_back(word);
    }
    emit_op(debug_names_, spirv::OpName, 0, 0, ops);
}

void SpirvBuilder::entry_point(u32 execution_model, u32 entry_point_id, 
                                const std::string& str, const std::vector<u32>& interface) {
    std::vector<u32> ops = {execution_model, entry_point_id};
    // Pack name
    size_t len = str.length() + 1;
    size_t words = (len + 3) / 4;
    for (size_t i = 0; i < words; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && i * 4 + j < len; j++) {
            word |= static_cast<u32>(str[i * 4 + j]) << (j * 8);
        }
        ops.push_back(word);
    }
    ops.insert(ops.end(), interface.begin(), interface.end());
    emit_op(entry_points_, spirv::OpEntryPoint, 0, 0, ops);
}

void SpirvBuilder::execution_mode(u32 entry_point_id, u32 mode, const std::vector<u32>& operands) {
    std::vector<u32> ops = {entry_point_id, mode};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(execution_modes_, spirv::OpExecutionMode, 0, 0, ops);
}

u32 SpirvBuilder::import_extension(const std::string& str) {
    u32 id = allocate_id();
    std::vector<u32> ops;
    size_t len = str.length() + 1;
    size_t words = (len + 3) / 4;
    for (size_t i = 0; i < words; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && i * 4 + j < len; j++) {
            word |= static_cast<u32>(str[i * 4 + j]) << (j * 8);
        }
        ops.push_back(word);
    }
    emit_op(ext_inst_imports_, spirv::OpExtInstImport, 0, id, ops);
    return id;
}

void SpirvBuilder::capability(u32 cap) {
    emit_op(capabilities_, spirv::OpCapability, 0, 0, {cap});
}

void SpirvBuilder::memory_model(u32 addressing, u32 memory) {
    emit_op(memory_model_, spirv::OpMemoryModel, 0, 0, {addressing, memory});
}

//=============================================================================
// Shader Translator Implementation
//=============================================================================

ShaderTranslator::ShaderTranslator() = default;
ShaderTranslator::~ShaderTranslator() = default;

Status ShaderTranslator::initialize(const std::string& cache_path) {
    cache_path_ = cache_path;
    load_cache();
    return Status::Ok;
}

void ShaderTranslator::shutdown() {
    save_cache();
}

std::vector<u32> ShaderTranslator::translate(const void* microcode, u32 size, ShaderType type) {
    // Check cache first
    u64 hash = compute_hash(microcode, size);
    const auto* cached = get_cached(hash);
    if (cached) {
        return *cached;
    }
    
    // Create translation context
    TranslationContext ctx;
    ctx.type = type;
    
    // Begin shader
    ctx.builder.begin(type);
    
    // Memory model
    ctx.builder.memory_model(0, 1); // Logical, GLSL450
    
    // Import GLSL extension
    ctx.glsl_ext = ctx.builder.import_extension("GLSL.std.450");
    
    // Setup types
    setup_types(ctx);
    
    // Setup inputs/outputs/uniforms
    setup_inputs(ctx);
    setup_outputs(ctx);
    setup_uniforms(ctx);
    
    // Create main function
    u32 void_func_type = ctx.builder.type_function(ctx.void_type, {});
    ctx.main_function = ctx.builder.allocate_id();
    ctx.builder.function_begin(ctx.void_type, void_func_type);
    
    // Entry label
    u32 entry_label = ctx.builder.allocate_id();
    ctx.builder.label(entry_label);
    
    // Allocate temporaries
    u32 temp_ptr_type = ctx.builder.type_pointer(spirv::StorageClassFunction, ctx.vec4_type);
    for (int i = 0; i < 128; i++) {
        ctx.temp_vars[i] = ctx.builder.variable(temp_ptr_type, spirv::StorageClassFunction);
    }
    
    // Parse and translate microcode
    const u32* words = static_cast<const u32*>(microcode);
    u32 num_words = size / 4;
    
    u32 pc = 0;
    while (pc < num_words) {
        // Check instruction type (control flow or exec)
        u32 control = words[pc];
        
        // Execute CF instruction
        u32 cf_type = (control >> 0) & 7;
        
        if (cf_type == 0) { // EXEC
            u32 addr = (control >> 3) & 0xFFF;
            u32 count = (control >> 15) & 0x3F;
            bool is_fetch = (control >> 21) & 1;
            
            for (u32 i = 0; i < count; i++) {
                if (is_fetch) {
                    XenosFetchInstruction fetch = decode_fetch_instruction(&words[addr + i * 3]);
                    translate_fetch_instruction(ctx, fetch);
                } else {
                    XenosAluInstruction alu = decode_alu_instruction(&words[addr + i * 3]);
                    translate_alu_instruction(ctx, alu);
                }
            }
        }
        
        pc++;
        
        // Simple limit to avoid infinite loops
        if (pc > 1024) break;
    }
    
    // Return
    ctx.builder.return_void();
    ctx.builder.function_end();
    
    // Add entry point
    std::vector<u32> interface_vars = {ctx.position_var};
    if (type == ShaderType::Vertex) {
        ctx.builder.entry_point(spirv::ExecutionModelVertex, ctx.main_function, "main", interface_vars);
    } else {
        ctx.builder.entry_point(spirv::ExecutionModelFragment, ctx.main_function, "main", interface_vars);
        ctx.builder.execution_mode(ctx.main_function, spirv::ExecutionModeOriginUpperLeft, {});
    }
    
    // Get result
    std::vector<u32> spirv = ctx.builder.end();
    
    // Cache it
    cache(hash, spirv);
    
    LOGI("Translated %s shader: %u microcode words -> %zu SPIR-V words",
         type == ShaderType::Vertex ? "vertex" : "pixel",
         num_words, spirv.size());
    
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
    u32 vec4_input_ptr = ctx.builder.type_pointer(spirv::StorageClassInput, ctx.vec4_type);
    
    if (ctx.type == ShaderType::Vertex) {
        // Vertex attributes
        ctx.vertex_id_var = ctx.builder.variable(
            ctx.builder.type_pointer(spirv::StorageClassInput, ctx.int_type),
            spirv::StorageClassInput
        );
        ctx.builder.decorate(ctx.vertex_id_var, spirv::DecorationBuiltIn, {spirv::BuiltInVertexIndex});
    } else {
        // Fragment inputs
        ctx.frag_coord_var = ctx.builder.variable(vec4_input_ptr, spirv::StorageClassInput);
        ctx.builder.decorate(ctx.frag_coord_var, spirv::DecorationBuiltIn, {spirv::BuiltInFragCoord});
    }
}

void ShaderTranslator::setup_outputs(TranslationContext& ctx) {
    u32 vec4_output_ptr = ctx.builder.type_pointer(spirv::StorageClassOutput, ctx.vec4_type);
    
    if (ctx.type == ShaderType::Vertex) {
        ctx.position_var = ctx.builder.variable(vec4_output_ptr, spirv::StorageClassOutput);
        ctx.builder.decorate(ctx.position_var, spirv::DecorationBuiltIn, {spirv::BuiltInPosition});
    } else {
        ctx.frag_color_var = ctx.builder.variable(vec4_output_ptr, spirv::StorageClassOutput);
        ctx.builder.decorate(ctx.frag_color_var, spirv::DecorationLocation, {0});
    }
}

void ShaderTranslator::setup_uniforms(TranslationContext& ctx) {
    // Create uniform buffer for constants (256 vec4 constants)
    u32 const_count = ctx.builder.const_uint(256);
    u32 const_array_type = ctx.builder.type_array(ctx.vec4_type, const_count);
    u32 const_struct_type = ctx.builder.type_struct({const_array_type});
    u32 const_ptr_type = ctx.builder.type_pointer(spirv::StorageClassUniform, const_struct_type);
    
    ctx.vertex_constants_var = ctx.builder.variable(const_ptr_type, spirv::StorageClassUniform);
    ctx.builder.decorate(ctx.vertex_constants_var, spirv::DecorationDescriptorSet, {0});
    ctx.builder.decorate(ctx.vertex_constants_var, spirv::DecorationBinding, {0});
    ctx.builder.decorate(const_struct_type, spirv::DecorationBlock, {});
    ctx.builder.member_decorate(const_struct_type, 0, spirv::DecorationOffset, {0});
}

void ShaderTranslator::translate_alu_instruction(TranslationContext& ctx, 
                                                  const XenosAluInstruction& inst) {
    // Get source values
    auto get_source = [&](const XenosAluInstruction::Source& src) -> u32 {
        u32 val;
        if (src.is_const) {
            // Load from constant buffer
            u32 idx = ctx.builder.const_uint(src.reg);
            u32 ptr = ctx.builder.access_chain(
                ctx.builder.type_pointer(spirv::StorageClassUniform, ctx.vec4_type),
                ctx.vertex_constants_var, {ctx.builder.const_uint(0), idx}
            );
            val = ctx.builder.load(ctx.vec4_type, ptr);
        } else {
            // Load from temporary
            val = ctx.builder.load(ctx.vec4_type, ctx.temp_vars[src.reg]);
        }
        
        // Apply swizzle
        std::vector<u32> swizzle_components;
        for (int i = 0; i < 4; i++) {
            swizzle_components.push_back((src.swizzle >> (i * 2)) & 3);
        }
        val = ctx.builder.vector_shuffle(ctx.vec4_type, val, val, swizzle_components);
        
        // Apply modifiers
        if (src.absolute) {
            val = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext, spirv::GLSLstd450FAbs, {val});
        }
        if (src.negate) {
            val = ctx.builder.f_negate(ctx.vec4_type, val);
        }
        
        return val;
    };
    
    u32 src1 = get_source(inst.src1);
    u32 src2 = get_source(inst.src2);
    u32 src3 = get_source(inst.src3);
    
    // Execute vector operation
    u32 vector_result = src1; // Default passthrough
    
    switch (inst.vector_op) {
        case AluVectorOp::ADDv:
            vector_result = ctx.builder.f_add(ctx.vec4_type, src1, src2);
            break;
        case AluVectorOp::MULv:
            vector_result = ctx.builder.f_mul(ctx.vec4_type, src1, src2);
            break;
        case AluVectorOp::MAXv:
            vector_result = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext, 
                                                  spirv::GLSLstd450FMax, {src1, src2});
            break;
        case AluVectorOp::MINv:
            vector_result = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450FMin, {src1, src2});
            break;
        case AluVectorOp::MULADDv:
            {
                u32 mul = ctx.builder.f_mul(ctx.vec4_type, src1, src2);
                vector_result = ctx.builder.f_add(ctx.vec4_type, mul, src3);
            }
            break;
        case AluVectorOp::DOT4v:
            {
                u32 dot = ctx.builder.allocate_id();
                std::vector<u32> ops = {src1, src2};
                ctx.builder.emit_op(ctx.builder.current_function_, spirv::OpDot, 
                                    ctx.float_type, dot, ops);
                // Splat to vec4
                vector_result = ctx.builder.composite_construct(ctx.vec4_type, 
                                                                {dot, dot, dot, dot});
            }
            break;
        case AluVectorOp::DOT3v:
            {
                // Extract xyz and compute dot3
                u32 src1_xyz = ctx.builder.vector_shuffle(ctx.vec3_type, src1, src1, {0, 1, 2});
                u32 src2_xyz = ctx.builder.vector_shuffle(ctx.vec3_type, src2, src2, {0, 1, 2});
                u32 dot = ctx.builder.allocate_id();
                std::vector<u32> ops = {src1_xyz, src2_xyz};
                ctx.builder.emit_op(ctx.builder.current_function_, spirv::OpDot,
                                    ctx.float_type, dot, ops);
                u32 zero = ctx.builder.const_float(0.0f);
                vector_result = ctx.builder.composite_construct(ctx.vec4_type,
                                                                {dot, dot, dot, zero});
            }
            break;
        case AluVectorOp::FRACv:
            vector_result = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Fract, {src1});
            break;
        case AluVectorOp::FLOORv:
            vector_result = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Floor, {src1});
            break;
        case AluVectorOp::TRUNCv:
            vector_result = ctx.builder.ext_inst(ctx.vec4_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Trunc, {src1});
            break;
        default:
            break;
    }
    
    // Store vector result
    if (inst.vector_write_mask && inst.vector_dest < 128) {
        ctx.builder.store(ctx.temp_vars[inst.vector_dest], vector_result);
    }
    
    // Execute scalar operation
    u32 scalar_src = ctx.builder.composite_extract(ctx.float_type, src1, {0}); // .x component
    u32 scalar_result = scalar_src;
    
    switch (inst.scalar_op) {
        case AluScalarOp::ADDs:
            {
                u32 src2_x = ctx.builder.composite_extract(ctx.float_type, src2, {0});
                scalar_result = ctx.builder.f_add(ctx.float_type, scalar_src, src2_x);
            }
            break;
        case AluScalarOp::MULs:
            {
                u32 src2_x = ctx.builder.composite_extract(ctx.float_type, src2, {0});
                scalar_result = ctx.builder.f_mul(ctx.float_type, scalar_src, src2_x);
            }
            break;
        case AluScalarOp::RECIP_IEEE:
            {
                u32 one = ctx.builder.const_float(1.0f);
                scalar_result = ctx.builder.f_div(ctx.float_type, one, scalar_src);
            }
            break;
        case AluScalarOp::RECIPSQ_IEEE:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450InverseSqrt, {scalar_src});
            break;
        case AluScalarOp::SQRT_IEEE:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Sqrt, {scalar_src});
            break;
        case AluScalarOp::EXP_IEEE:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Exp2, {scalar_src});
            break;
        case AluScalarOp::LOG_IEEE:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Log2, {scalar_src});
            break;
        case AluScalarOp::SIN:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Sin, {scalar_src});
            break;
        case AluScalarOp::COS:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Cos, {scalar_src});
            break;
        case AluScalarOp::FRACs:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Fract, {scalar_src});
            break;
        case AluScalarOp::FLOORs:
            scalar_result = ctx.builder.ext_inst(ctx.float_type, ctx.glsl_ext,
                                                  spirv::GLSLstd450Floor, {scalar_src});
            break;
        default:
            break;
    }
    
    // Store scalar result
    if (inst.scalar_write_mask && inst.scalar_dest < 128) {
        u32 vec_result = ctx.builder.composite_construct(ctx.vec4_type,
            {scalar_result, scalar_result, scalar_result, scalar_result});
        ctx.builder.store(ctx.temp_vars[inst.scalar_dest], vec_result);
    }
    
    // Handle exports
    if (inst.export_data) {
        if (ctx.type == ShaderType::Vertex && inst.export_reg == 62) {
            // Export to position
            ctx.builder.store(ctx.position_var, vector_result);
        } else if (ctx.type == ShaderType::Pixel && inst.export_reg == 0) {
            // Export to color
            ctx.builder.store(ctx.frag_color_var, vector_result);
        }
    }
}

void ShaderTranslator::translate_fetch_instruction(TranslationContext& ctx,
                                                    const XenosFetchInstruction& inst) {
    // Texture fetch implementation
    if (inst.op == FetchOp::TextureFetch) {
        // Would set up texture sampling here
        // For now, output a constant color
        u32 one = ctx.builder.const_float(1.0f);
        u32 color = ctx.builder.composite_construct(ctx.vec4_type, {one, one, one, one});
        ctx.builder.store(ctx.temp_vars[inst.dest_reg], color);
    }
    else if (inst.op == FetchOp::VertexFetch) {
        // Vertex fetch - would load from vertex buffer
        // For now, output zeros
        u32 zero = ctx.builder.const_float(0.0f);
        u32 vertex = ctx.builder.composite_construct(ctx.vec4_type, {zero, zero, zero, zero});
        ctx.builder.store(ctx.temp_vars[inst.dest_reg], vertex);
    }
}

const std::vector<u32>* ShaderTranslator::get_cached(u64 hash) const {
    auto it = cache_.find(hash);
    return it != cache_.end() ? &it->second : nullptr;
}

void ShaderTranslator::cache(u64 hash, std::vector<u32> spirv) {
    cache_[hash] = std::move(spirv);
}

void ShaderTranslator::save_cache() {
    if (cache_path_.empty()) return;
    // Would save to disk here
}

void ShaderTranslator::load_cache() {
    if (cache_path_.empty()) return;
    // Would load from disk here
}

u64 ShaderTranslator::compute_hash(const void* data, u32 size) {
    // FNV-1a hash
    const u8* bytes = static_cast<const u8*>(data);
    u64 hash = 14695981039346656037ULL;
    for (u32 i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace x360mu

