/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * SPIR-V Builder Implementation
 * Generates SPIR-V binary from shader translation
 */

#include "shader_translator.h"
#include <cstring>
#include <algorithm>

namespace x360mu {

// SPIR-V magic number and version
constexpr u32 SPIRV_MAGIC = 0x07230203;
constexpr u32 SPIRV_VERSION = 0x00010300;  // SPIR-V 1.3
constexpr u32 SPIRV_GENERATOR = 0x00000000;

// SPIR-V opcodes
namespace spv {
    // Misc
    constexpr u32 OpNop = 0;
    constexpr u32 OpUndef = 1;
    constexpr u32 OpSourceContinued = 2;
    constexpr u32 OpSource = 3;
    constexpr u32 OpSourceExtension = 4;
    constexpr u32 OpName = 5;
    constexpr u32 OpMemberName = 6;
    constexpr u32 OpString = 7;
    constexpr u32 OpLine = 8;
    
    // Decorations
    constexpr u32 OpDecorate = 71;
    constexpr u32 OpMemberDecorate = 72;
    
    // Types
    constexpr u32 OpTypeVoid = 19;
    constexpr u32 OpTypeBool = 20;
    constexpr u32 OpTypeInt = 21;
    constexpr u32 OpTypeFloat = 22;
    constexpr u32 OpTypeVector = 23;
    constexpr u32 OpTypeMatrix = 24;
    constexpr u32 OpTypeImage = 25;
    constexpr u32 OpTypeSampler = 26;
    constexpr u32 OpTypeSampledImage = 27;
    constexpr u32 OpTypeArray = 28;
    constexpr u32 OpTypeRuntimeArray = 29;
    constexpr u32 OpTypeStruct = 30;
    constexpr u32 OpTypeOpaque = 31;
    constexpr u32 OpTypePointer = 32;
    constexpr u32 OpTypeFunction = 33;
    
    // Constants
    constexpr u32 OpConstantTrue = 41;
    constexpr u32 OpConstantFalse = 42;
    constexpr u32 OpConstant = 43;
    constexpr u32 OpConstantComposite = 44;
    
    // Memory
    constexpr u32 OpVariable = 59;
    constexpr u32 OpLoad = 61;
    constexpr u32 OpStore = 62;
    constexpr u32 OpAccessChain = 65;
    
    // Function
    constexpr u32 OpFunction = 54;
    constexpr u32 OpFunctionParameter = 55;
    constexpr u32 OpFunctionEnd = 56;
    constexpr u32 OpFunctionCall = 57;
    
    // Control flow
    constexpr u32 OpLabel = 248;
    constexpr u32 OpBranch = 249;
    constexpr u32 OpBranchConditional = 250;
    constexpr u32 OpSwitch = 251;
    constexpr u32 OpKill = 252;
    constexpr u32 OpReturn = 253;
    constexpr u32 OpReturnValue = 254;
    constexpr u32 OpLoopMerge = 246;
    constexpr u32 OpSelectionMerge = 247;
    
    // Arithmetic
    constexpr u32 OpIAdd = 128;
    constexpr u32 OpFAdd = 129;
    constexpr u32 OpISub = 130;
    constexpr u32 OpFSub = 131;
    constexpr u32 OpIMul = 132;
    constexpr u32 OpFMul = 133;
    constexpr u32 OpFDiv = 136;
    constexpr u32 OpFMod = 141;
    constexpr u32 OpFNegate = 127;
    
    // Vector
    constexpr u32 OpVectorShuffle = 79;
    constexpr u32 OpCompositeConstruct = 80;
    constexpr u32 OpCompositeExtract = 81;
    constexpr u32 OpCompositeInsert = 82;
    
    // Comparison
    constexpr u32 OpFOrdEqual = 180;
    constexpr u32 OpFOrdNotEqual = 182;
    constexpr u32 OpFOrdLessThan = 184;
    constexpr u32 OpFOrdGreaterThan = 186;
    constexpr u32 OpFOrdLessThanEqual = 188;
    constexpr u32 OpFOrdGreaterThanEqual = 190;
    
    // Logical
    constexpr u32 OpSelect = 169;
    
    // Image
    constexpr u32 OpImageSampleImplicitLod = 87;
    constexpr u32 OpImageSampleExplicitLod = 88;
    
    // Extensions
    constexpr u32 OpExtInstImport = 11;
    constexpr u32 OpExtInst = 12;
    
    // Module
    constexpr u32 OpCapability = 17;
    constexpr u32 OpMemoryModel = 14;
    constexpr u32 OpEntryPoint = 15;
    constexpr u32 OpExecutionMode = 16;
    
    // Capability values
    constexpr u32 CapabilityShader = 1;
    
    // Execution model
    constexpr u32 ExecutionModelVertex = 0;
    constexpr u32 ExecutionModelFragment = 4;
    
    // Execution mode
    constexpr u32 ExecutionModeOriginUpperLeft = 7;
    
    // Addressing/Memory model
    constexpr u32 AddressingModelLogical = 0;
    constexpr u32 MemoryModelGLSL450 = 1;
    
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
    constexpr u32 DecorationFlat = 14;
    constexpr u32 DecorationNoPerspective = 13;
    constexpr u32 DecorationBlock = 2;
    constexpr u32 DecorationOffset = 35;
    
    // BuiltIn values
    constexpr u32 BuiltInPosition = 0;
    constexpr u32 BuiltInPointSize = 1;
    constexpr u32 BuiltInVertexIndex = 42;
    constexpr u32 BuiltInInstanceIndex = 43;
    constexpr u32 BuiltInFragCoord = 15;
    constexpr u32 BuiltInFrontFacing = 17;
    constexpr u32 BuiltInFragDepth = 22;
    
    // GLSL.std.450 extended instructions
    constexpr u32 GLSLstd450Round = 1;
    constexpr u32 GLSLstd450Trunc = 3;
    constexpr u32 GLSLstd450FAbs = 4;
    constexpr u32 GLSLstd450Floor = 8;
    constexpr u32 GLSLstd450Fract = 10;
    constexpr u32 GLSLstd450Sqrt = 31;
    constexpr u32 GLSLstd450InverseSqrt = 32;
    constexpr u32 GLSLstd450Exp = 27;
    constexpr u32 GLSLstd450Exp2 = 29;
    constexpr u32 GLSLstd450Log = 28;
    constexpr u32 GLSLstd450Log2 = 30;
    constexpr u32 GLSLstd450Sin = 13;
    constexpr u32 GLSLstd450Cos = 14;
    constexpr u32 GLSLstd450Pow = 26;
    constexpr u32 GLSLstd450FMin = 37;
    constexpr u32 GLSLstd450FMax = 40;
    constexpr u32 GLSLstd450FClamp = 43;
    constexpr u32 GLSLstd450FMix = 46;
    constexpr u32 GLSLstd450Length = 66;
    constexpr u32 GLSLstd450Normalize = 69;
    constexpr u32 GLSLstd450Cross = 68;
    constexpr u32 GLSLstd450Reflect = 71;
    
    // Dimension
    constexpr u32 Dim1D = 0;
    constexpr u32 Dim2D = 1;
    constexpr u32 Dim3D = 2;
    constexpr u32 DimCube = 3;
}

SpirvBuilder::SpirvBuilder() {
    next_id_ = 1;
}

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
    
    // Add shader capability
    capability(spv::CapabilityShader);
    
    // Set memory model
    memory_model(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
}

std::vector<u32> SpirvBuilder::end() {
    std::vector<u32> result;
    
    // Calculate bound (highest ID + 1)
    u32 bound = next_id_;
    
    // Header
    result.push_back(SPIRV_MAGIC);
    result.push_back(SPIRV_VERSION);
    result.push_back(SPIRV_GENERATOR);
    result.push_back(bound);
    result.push_back(0);  // Reserved
    
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

void SpirvBuilder::emit_op(std::vector<u32>& target, u32 opcode, u32 result_type, u32 result_id,
                           const std::vector<u32>& operands) {
    u32 word_count = 1;  // Opcode word
    if (result_type != 0) word_count++;
    if (result_id != 0) word_count++;
    word_count += operands.size();
    
    target.push_back((word_count << 16) | opcode);
    if (result_type != 0) target.push_back(result_type);
    if (result_id != 0) target.push_back(result_id);
    for (u32 op : operands) {
        target.push_back(op);
    }
}

// Type declarations
u32 SpirvBuilder::type_void() {
    u64 key = 0x1000000000000000ULL;
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeVoid, 0, id, {});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_bool() {
    u64 key = 0x2000000000000000ULL;
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeBool, 0, id, {});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_int(u32 width, bool signed_) {
    u64 key = 0x3000000000000000ULL | (static_cast<u64>(width) << 32) | (signed_ ? 1 : 0);
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeInt, 0, id, {width, signed_ ? 1u : 0u});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_float(u32 width) {
    u64 key = 0x4000000000000000ULL | static_cast<u64>(width);
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeFloat, 0, id, {width});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_vector(u32 component_type, u32 count) {
    u64 key = 0x5000000000000000ULL | (static_cast<u64>(component_type) << 16) | count;
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeVector, 0, id, {component_type, count});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_matrix(u32 column_type, u32 columns) {
    u64 key = 0x6000000000000000ULL | (static_cast<u64>(column_type) << 16) | columns;
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeMatrix, 0, id, {column_type, columns});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_array(u32 element_type, u32 length) {
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeArray, 0, id, {element_type, length});
    return id;
}

u32 SpirvBuilder::type_struct(const std::vector<u32>& members) {
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeStruct, 0, id, members);
    return id;
}

u32 SpirvBuilder::type_pointer(u32 storage_class, u32 type) {
    u64 key = 0x7000000000000000ULL | (static_cast<u64>(storage_class) << 32) | type;
    auto it = type_cache_.find(key);
    if (it != type_cache_.end()) return it->second;
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypePointer, 0, id, {storage_class, type});
    type_cache_[key] = id;
    return id;
}

u32 SpirvBuilder::type_function(u32 return_type, const std::vector<u32>& params) {
    std::vector<u32> ops = {return_type};
    ops.insert(ops.end(), params.begin(), params.end());
    
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeFunction, 0, id, ops);
    return id;
}

u32 SpirvBuilder::type_image(u32 sampled_type, u32 dim, bool depth, bool arrayed, bool ms, u32 sampled) {
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeImage, 0, id, 
            {sampled_type, dim, depth ? 1u : 0u, arrayed ? 1u : 0u, ms ? 1u : 0u, sampled, 0});
    return id;
}

u32 SpirvBuilder::type_sampled_image(u32 image_type) {
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpTypeSampledImage, 0, id, {image_type});
    return id;
}

// Constants
u32 SpirvBuilder::const_bool(bool value) {
    u32 type = type_bool();
    u32 id = allocate_id();
    emit_op(types_constants_, value ? spv::OpConstantTrue : spv::OpConstantFalse, type, id, {});
    return id;
}

u32 SpirvBuilder::const_int(s32 value) {
    u32 type = type_int(32, true);
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpConstant, type, id, {static_cast<u32>(value)});
    return id;
}

u32 SpirvBuilder::const_uint(u32 value) {
    u32 type = type_int(32, false);
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpConstant, type, id, {value});
    return id;
}

u32 SpirvBuilder::const_float(f32 value) {
    u32 type = type_float(32);
    u32 id = allocate_id();
    u32 bits;
    memcpy(&bits, &value, 4);
    emit_op(types_constants_, spv::OpConstant, type, id, {bits});
    return id;
}

u32 SpirvBuilder::const_composite(u32 type, const std::vector<u32>& constituents) {
    u32 id = allocate_id();
    emit_op(types_constants_, spv::OpConstantComposite, type, id, constituents);
    return id;
}

// Variables
u32 SpirvBuilder::variable(u32 pointer_type, u32 storage_class, u32 initializer) {
    u32 id = allocate_id();
    std::vector<u32> ops = {storage_class};
    if (initializer != 0) {
        ops.push_back(initializer);
    }
    
    if (storage_class == spv::StorageClassFunction) {
        emit_op(current_function_, spv::OpVariable, pointer_type, id, ops);
    } else {
        emit_op(globals_, spv::OpVariable, pointer_type, id, ops);
    }
    return id;
}

// Function control
void SpirvBuilder::function_begin(u32 return_type, u32 function_type) {
    current_function_.clear();
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFunction, return_type, id, {0, function_type});
}

void SpirvBuilder::function_end() {
    emit_op(current_function_, spv::OpFunctionEnd, 0, 0, {});
    functions_.insert(functions_.end(), current_function_.begin(), current_function_.end());
    current_function_.clear();
}

void SpirvBuilder::label(u32 id) {
    emit_op(current_function_, spv::OpLabel, 0, id, {});
}

void SpirvBuilder::return_void() {
    emit_op(current_function_, spv::OpReturn, 0, 0, {});
}

void SpirvBuilder::return_value(u32 value) {
    emit_op(current_function_, spv::OpReturnValue, 0, 0, {value});
}

// Memory
u32 SpirvBuilder::load(u32 result_type, u32 pointer) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpLoad, result_type, id, {pointer});
    return id;
}

void SpirvBuilder::store(u32 pointer, u32 value) {
    emit_op(current_function_, spv::OpStore, 0, 0, {pointer, value});
}

u32 SpirvBuilder::access_chain(u32 result_type, u32 base, const std::vector<u32>& indices) {
    u32 id = allocate_id();
    std::vector<u32> ops = {base};
    ops.insert(ops.end(), indices.begin(), indices.end());
    emit_op(current_function_, spv::OpAccessChain, result_type, id, ops);
    return id;
}

// Arithmetic
u32 SpirvBuilder::f_add(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFAdd, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_sub(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFSub, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_mul(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFMul, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_div(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFDiv, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_negate(u32 type, u32 a) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFNegate, type, id, {a});
    return id;
}

u32 SpirvBuilder::f_mod(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFMod, type, id, {a, b});
    return id;
}

// Extended instructions
u32 SpirvBuilder::ext_inst(u32 type, u32 set, u32 instruction, const std::vector<u32>& operands) {
    u32 id = allocate_id();
    std::vector<u32> ops = {set, instruction};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(current_function_, spv::OpExtInst, type, id, ops);
    return id;
}

// Vector operations
u32 SpirvBuilder::vector_shuffle(u32 type, u32 v1, u32 v2, const std::vector<u32>& components) {
    u32 id = allocate_id();
    std::vector<u32> ops = {v1, v2};
    ops.insert(ops.end(), components.begin(), components.end());
    emit_op(current_function_, spv::OpVectorShuffle, type, id, ops);
    return id;
}

u32 SpirvBuilder::composite_extract(u32 type, u32 composite, const std::vector<u32>& indices) {
    u32 id = allocate_id();
    std::vector<u32> ops = {composite};
    ops.insert(ops.end(), indices.begin(), indices.end());
    emit_op(current_function_, spv::OpCompositeExtract, type, id, ops);
    return id;
}

u32 SpirvBuilder::composite_insert(u32 type, u32 object, u32 composite, const std::vector<u32>& indices) {
    u32 id = allocate_id();
    std::vector<u32> ops = {object, composite};
    ops.insert(ops.end(), indices.begin(), indices.end());
    emit_op(current_function_, spv::OpCompositeInsert, type, id, ops);
    return id;
}

u32 SpirvBuilder::composite_construct(u32 type, const std::vector<u32>& constituents) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpCompositeConstruct, type, id, constituents);
    return id;
}

// Comparison
u32 SpirvBuilder::f_ord_equal(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdEqual, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_ord_not_equal(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdNotEqual, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_ord_less_than(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdLessThan, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_ord_greater_than(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdGreaterThan, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_ord_less_than_equal(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdLessThanEqual, type, id, {a, b});
    return id;
}

u32 SpirvBuilder::f_ord_greater_than_equal(u32 type, u32 a, u32 b) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpFOrdGreaterThanEqual, type, id, {a, b});
    return id;
}

// Control flow
u32 SpirvBuilder::select(u32 type, u32 condition, u32 true_val, u32 false_val) {
    u32 id = allocate_id();
    emit_op(current_function_, spv::OpSelect, type, id, {condition, true_val, false_val});
    return id;
}

void SpirvBuilder::branch(u32 target) {
    emit_op(current_function_, spv::OpBranch, 0, 0, {target});
}

void SpirvBuilder::branch_conditional(u32 condition, u32 true_label, u32 false_label) {
    emit_op(current_function_, spv::OpBranchConditional, 0, 0, {condition, true_label, false_label});
}

void SpirvBuilder::loop_merge(u32 merge_block, u32 continue_target, u32 control) {
    emit_op(current_function_, spv::OpLoopMerge, 0, 0, {merge_block, continue_target, control});
}

void SpirvBuilder::selection_merge(u32 merge_block, u32 control) {
    emit_op(current_function_, spv::OpSelectionMerge, 0, 0, {merge_block, control});
}

// Texture
u32 SpirvBuilder::image_sample(u32 type, u32 sampled_image, u32 coord, u32 bias) {
    u32 id = allocate_id();
    if (bias != 0) {
        emit_op(current_function_, spv::OpImageSampleImplicitLod, type, id, 
                {sampled_image, coord, 0x1, bias});  // Bias operand mask
    } else {
        emit_op(current_function_, spv::OpImageSampleImplicitLod, type, id, 
                {sampled_image, coord});
    }
    return id;
}

// Decorations
void SpirvBuilder::decorate(u32 target, u32 decoration, const std::vector<u32>& operands) {
    std::vector<u32> ops = {target, decoration};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(decorations_, spv::OpDecorate, 0, 0, ops);
}

void SpirvBuilder::member_decorate(u32 type, u32 member, u32 decoration, const std::vector<u32>& operands) {
    std::vector<u32> ops = {type, member, decoration};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(decorations_, spv::OpMemberDecorate, 0, 0, ops);
}

// Debug
void SpirvBuilder::name(u32 target, const std::string& name) {
    std::vector<u32> ops = {target};
    
    // Pack string into words
    size_t len = name.size() + 1;  // Include null terminator
    size_t word_count = (len + 3) / 4;
    
    for (size_t i = 0; i < word_count; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && (i * 4 + j) < len; j++) {
            size_t idx = i * 4 + j;
            u8 c = idx < name.size() ? name[idx] : 0;
            word |= c << (j * 8);
        }
        ops.push_back(word);
    }
    
    emit_op(debug_names_, spv::OpName, 0, 0, ops);
}

void SpirvBuilder::member_name(u32 type, u32 member, const std::string& name) {
    // Similar to name() but with member index
    std::vector<u32> ops = {type, member};
    
    size_t len = name.size() + 1;
    size_t word_count = (len + 3) / 4;
    
    for (size_t i = 0; i < word_count; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && (i * 4 + j) < len; j++) {
            size_t idx = i * 4 + j;
            u8 c = idx < name.size() ? name[idx] : 0;
            word |= c << (j * 8);
        }
        ops.push_back(word);
    }
    
    emit_op(debug_names_, spv::OpMemberName, 0, 0, ops);
}

// Entry point
void SpirvBuilder::entry_point(u32 execution_model, u32 entry_point_id, const std::string& name_str,
                               const std::vector<u32>& interface_ids) {
    std::vector<u32> ops = {execution_model, entry_point_id};
    
    // Pack name
    size_t len = name_str.size() + 1;
    size_t word_count = (len + 3) / 4;
    
    for (size_t i = 0; i < word_count; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && (i * 4 + j) < len; j++) {
            size_t idx = i * 4 + j;
            u8 c = idx < name_str.size() ? name_str[idx] : 0;
            word |= c << (j * 8);
        }
        ops.push_back(word);
    }
    
    ops.insert(ops.end(), interface_ids.begin(), interface_ids.end());
    emit_op(entry_points_, spv::OpEntryPoint, 0, 0, ops);
}

void SpirvBuilder::execution_mode(u32 entry_point_id, u32 mode, const std::vector<u32>& operands) {
    std::vector<u32> ops = {entry_point_id, mode};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit_op(execution_modes_, spv::OpExecutionMode, 0, 0, ops);
}

// Extensions
u32 SpirvBuilder::import_extension(const std::string& name) {
    u32 id = allocate_id();
    
    std::vector<u32> ops;
    size_t len = name.size() + 1;
    size_t word_count = (len + 3) / 4;
    
    for (size_t i = 0; i < word_count; i++) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && (i * 4 + j) < len; j++) {
            size_t idx = i * 4 + j;
            u8 c = idx < name.size() ? name[idx] : 0;
            word |= c << (j * 8);
        }
        ops.push_back(word);
    }
    
    u32 word_count_total = 2 + ops.size();
    ext_inst_imports_.push_back((word_count_total << 16) | spv::OpExtInstImport);
    ext_inst_imports_.push_back(id);
    ext_inst_imports_.insert(ext_inst_imports_.end(), ops.begin(), ops.end());
    
    return id;
}

void SpirvBuilder::capability(u32 cap) {
    emit_op(capabilities_, spv::OpCapability, 0, 0, {cap});
}

void SpirvBuilder::memory_model(u32 addressing, u32 memory) {
    emit_op(memory_model_, spv::OpMemoryModel, 0, 0, {addressing, memory});
}

} // namespace x360mu

