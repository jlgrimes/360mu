/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * SPIR-V Builder Implementation using glslang
 * Generates SPIR-V binary from shader translation
 */

#include "shader_translator.h"
#include <cstring>
#include <algorithm>

#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/SpvBuilder.h>
#include <glslang/SPIRV/GLSL.std.450.h>

namespace x360mu {

// Map our storage class enum values to SPIR-V
namespace StorageClass {
    constexpr u32 UniformConstant = 0;
    constexpr u32 Input = 1;
    constexpr u32 Uniform = 2;
    constexpr u32 Output = 3;
    constexpr u32 Private = 6;
    constexpr u32 Function = 7;
}

// Decoration values
namespace Decoration {
    constexpr u32 Block = 2;
    constexpr u32 BuiltIn = 11;
    constexpr u32 NoPerspective = 13;
    constexpr u32 Flat = 14;
    constexpr u32 Location = 30;
    constexpr u32 Binding = 33;
    constexpr u32 DescriptorSet = 34;
    constexpr u32 Offset = 35;
    constexpr u32 ArrayStride = 6;
}

// BuiltIn values
namespace BuiltIn {
    constexpr u32 Position = 0;
    constexpr u32 PointSize = 1;
    constexpr u32 FragCoord = 15;
    constexpr u32 FrontFacing = 17;
    constexpr u32 FragDepth = 22;
    constexpr u32 VertexIndex = 42;
    constexpr u32 InstanceIndex = 43;
}

// Execution modes
namespace ExecutionMode {
    constexpr u32 OriginUpperLeft = 7;
    constexpr u32 DepthReplacing = 12;
}

// Implementation using glslang's SpvBuilder
class SpirvBuilderImpl {
public:
    SpirvBuilderImpl() 
        : builder_(spv::SpvBuildLogger::Quiet)
    {
        // Initialize glslang once
        static bool glslang_initialized = false;
        if (!glslang_initialized) {
            glslang::InitializeProcess();
            glslang_initialized = true;
        }
    }
    
    ~SpirvBuilderImpl() = default;
    
    spv::Builder& get() { return builder_; }
    
    void begin(ShaderType type) {
        // Reset builder for new shader
        builder_.clearAccessChain();
        
        // Set source language
        builder_.setSource(spv::SourceLanguageUnknown, 0);
        
        // Add capability
        builder_.addCapability(spv::CapabilityShader);
        
        // Set memory model
        builder_.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
        
        shader_type_ = type;
    }
    
    std::vector<u32> end() {
        std::vector<u32> spirv;
        builder_.dump(spirv);
        return spirv;
    }
    
    ShaderType shader_type_ = ShaderType::Vertex;
    
private:
    spv::Builder builder_;
};

// Global impl storage per thread
static thread_local std::unique_ptr<SpirvBuilderImpl> tls_impl;

static SpirvBuilderImpl& get_impl() {
    if (!tls_impl) {
        tls_impl = std::make_unique<SpirvBuilderImpl>();
    }
    return *tls_impl;
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
    
    auto& impl = get_impl();
    impl.begin(type);
    
    // Add shader capability using our interface
    capability(1); // CapabilityShader
    
    // Set memory model
    memory_model(0, 1); // AddressingModelLogical, MemoryModelGLSL450
}

std::vector<u32> SpirvBuilder::end() {
    auto& impl = get_impl();
    return impl.end();
}

// Type declarations - delegate to glslang
u32 SpirvBuilder::type_void() {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeVoidType();
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_bool() {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeBoolType();
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_int(u32 width, bool signed_) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeIntType(width);
    if (!signed_) {
        id = builder.makeUintType(width);
    }
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_float(u32 width) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeFloatType(width);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_vector(u32 component_type, u32 count) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeVectorType(static_cast<spv::Id>(component_type), count);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_matrix(u32 column_type, u32 columns) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeMatrixType(static_cast<spv::Id>(column_type), columns, 0);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_array(u32 element_type, u32 length) {
    auto& builder = get_impl().get();
    spv::Id length_id = builder.makeUintConstant(length);
    spv::Id id = builder.makeArrayType(static_cast<spv::Id>(element_type), length_id, 0);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_runtime_array(u32 element_type) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeRuntimeArray(static_cast<spv::Id>(element_type));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_struct(const std::vector<u32>& members) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> member_ids;
    for (u32 m : members) {
        member_ids.push_back(static_cast<spv::Id>(m));
    }
    spv::Id id = builder.makeStructType(member_ids, "");
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_pointer(u32 storage_class, u32 type) {
    auto& builder = get_impl().get();
    spv::StorageClass sc = static_cast<spv::StorageClass>(storage_class);
    spv::Id id = builder.makePointer(sc, static_cast<spv::Id>(type));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_function(u32 return_type, const std::vector<u32>& params) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> param_ids;
    for (u32 p : params) {
        param_ids.push_back(static_cast<spv::Id>(p));
    }
    spv::Id id = builder.makeFunctionType(static_cast<spv::Id>(return_type), param_ids);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_image(u32 sampled_type, u32 dim, bool depth, bool arrayed, bool ms, u32 sampled) {
    auto& builder = get_impl().get();
    spv::Dim spv_dim = static_cast<spv::Dim>(dim);
    spv::Id id = builder.makeImageType(
        static_cast<spv::Id>(sampled_type),
        spv_dim,
        depth,
        arrayed,
        ms,
        sampled,
        spv::ImageFormatUnknown
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_sampled_image(u32 image_type) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeSampledImageType(static_cast<spv::Id>(image_type));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::type_sampler() {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeSamplerType();
    return static_cast<u32>(id);
}

// Constants
u32 SpirvBuilder::const_bool(bool value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeBoolConstant(value);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::const_int(s32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeIntConstant(value);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::const_uint(u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeUintConstant(value);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::const_float(f32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.makeFloatConstant(value);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::const_composite(u32 type, const std::vector<u32>& constituents) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> ids;
    for (u32 c : constituents) {
        ids.push_back(static_cast<spv::Id>(c));
    }
    spv::Id id = builder.makeCompositeConstant(static_cast<spv::Id>(type), ids);
    return static_cast<u32>(id);
}

// Variables
u32 SpirvBuilder::variable(u32 pointer_type, u32 storage_class, u32 initializer) {
    auto& builder = get_impl().get();
    spv::StorageClass sc = static_cast<spv::StorageClass>(storage_class);
    spv::Id init_id = initializer ? static_cast<spv::Id>(initializer) : spv::NoResult;
    spv::Id id = builder.createVariable(sc, static_cast<spv::Id>(pointer_type), "", init_id);
    return static_cast<u32>(id);
}

// Function control
void SpirvBuilder::function_begin(u32 return_type, u32 function_type) {
    auto& builder = get_impl().get();
    builder.makeFunctionEntry(
        spv::NoPrecision,
        static_cast<spv::Id>(return_type),
        "main",
        {},
        {},
        nullptr
    );
}

void SpirvBuilder::function_end() {
    auto& builder = get_impl().get();
    builder.leaveFunction();
}

void SpirvBuilder::label(u32 id) {
    auto& builder = get_impl().get();
    spv::Block* block = new spv::Block(static_cast<spv::Id>(id), *builder.getBuildPoint()->getParent());
    builder.setBuildPoint(block);
}

void SpirvBuilder::return_void() {
    auto& builder = get_impl().get();
    builder.makeReturn(false);
}

void SpirvBuilder::return_value(u32 value) {
    auto& builder = get_impl().get();
    builder.makeReturn(false, static_cast<spv::Id>(value));
}

// Memory operations
u32 SpirvBuilder::load(u32 result_type, u32 pointer) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createLoad(static_cast<spv::Id>(pointer), spv::NoPrecision);
    return static_cast<u32>(id);
}

void SpirvBuilder::store(u32 pointer, u32 value) {
    auto& builder = get_impl().get();
    builder.createStore(static_cast<spv::Id>(value), static_cast<spv::Id>(pointer));
}

u32 SpirvBuilder::access_chain(u32 result_type, u32 base, const std::vector<u32>& indices) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> idx_ids;
    for (u32 i : indices) {
        idx_ids.push_back(static_cast<spv::Id>(i));
    }
    spv::Id id = builder.createAccessChain(
        spv::StorageClassFunction,
        static_cast<spv::Id>(base),
        idx_ids
    );
    return static_cast<u32>(id);
}

// Arithmetic
u32 SpirvBuilder::f_add(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFAdd, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_sub(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFSub, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_mul(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFMul, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_div(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFDiv, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_negate(u32 type, u32 a) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpFNegate, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(a));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_mod(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFMod, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::i_add(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpIAdd, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::i_sub(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpISub, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::i_mul(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpIMul, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

// Conversions
u32 SpirvBuilder::convert_f_to_s(u32 type, u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpConvertFToS, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(value));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::convert_s_to_f(u32 type, u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpConvertSToF, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(value));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::convert_f_to_u(u32 type, u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpConvertFToU, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(value));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::convert_u_to_f(u32 type, u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpConvertUToF, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(value));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::bitcast(u32 type, u32 value) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpBitcast, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(value));
    return static_cast<u32>(id);
}

// Extended instructions
u32 SpirvBuilder::ext_inst(u32 type, u32 set, u32 instruction, const std::vector<u32>& operands) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> ops;
    for (u32 o : operands) {
        ops.push_back(static_cast<spv::Id>(o));
    }
    spv::Id id = builder.createBuiltinCall(
        static_cast<spv::Id>(type),
        static_cast<spv::Id>(set),
        instruction,
        ops
    );
    return static_cast<u32>(id);
}

// Vector operations
u32 SpirvBuilder::vector_shuffle(u32 type, u32 v1, u32 v2, const std::vector<u32>& components) {
    auto& builder = get_impl().get();
    std::vector<int> comps;
    for (u32 c : components) {
        comps.push_back(static_cast<int>(c));
    }
    spv::Id id = builder.createRvalueSwizzle(
        spv::NoPrecision,
        static_cast<spv::Id>(type),
        static_cast<spv::Id>(v1),
        comps
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::composite_extract(u32 type, u32 composite, const std::vector<u32>& indices) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createCompositeExtract(
        static_cast<spv::Id>(composite),
        static_cast<spv::Id>(type),
        indices[0]
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::composite_insert(u32 type, u32 object, u32 composite, const std::vector<u32>& indices) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createCompositeInsert(
        static_cast<spv::Id>(object),
        static_cast<spv::Id>(composite),
        static_cast<spv::Id>(type),
        indices[0]
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::composite_construct(u32 type, const std::vector<u32>& constituents) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> ids;
    for (u32 c : constituents) {
        ids.push_back(static_cast<spv::Id>(c));
    }
    spv::Id id = builder.createCompositeConstruct(static_cast<spv::Id>(type), ids);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::vector_extract_dynamic(u32 type, u32 vector, u32 index) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createVectorExtractDynamic(
        static_cast<spv::Id>(vector),
        static_cast<spv::Id>(type),
        static_cast<spv::Id>(index)
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::vector_insert_dynamic(u32 type, u32 vector, u32 component, u32 index) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createVectorInsertDynamic(
        static_cast<spv::Id>(vector),
        static_cast<spv::Id>(type),
        static_cast<spv::Id>(component),
        static_cast<spv::Id>(index)
    );
    return static_cast<u32>(id);
}

u32 SpirvBuilder::dot(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpDot, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

// Comparison operations
u32 SpirvBuilder::f_ord_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_ord_not_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdNotEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_ord_less_than(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdLessThan, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_ord_greater_than(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdGreaterThan, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_ord_less_than_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdLessThanEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::f_ord_greater_than_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpFOrdGreaterThanEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::i_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpIEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::i_not_equal(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpINotEqual, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

// Logical operations
u32 SpirvBuilder::logical_and(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpLogicalAnd, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::logical_or(u32 type, u32 a, u32 b) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpLogicalOr, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(a), static_cast<spv::Id>(b));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::logical_not(u32 type, u32 a) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpLogicalNot, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(a));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::any(u32 type, u32 vector) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpAny, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(vector));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::all(u32 type, u32 vector) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createUnaryOp(spv::OpAll, static_cast<spv::Id>(type),
                                        static_cast<spv::Id>(vector));
    return static_cast<u32>(id);
}

// Control flow
u32 SpirvBuilder::select(u32 type, u32 condition, u32 true_val, u32 false_val) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createTriOp(spv::OpSelect, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(condition),
                                      static_cast<spv::Id>(true_val),
                                      static_cast<spv::Id>(false_val));
    return static_cast<u32>(id);
}

void SpirvBuilder::branch(u32 target) {
    auto& builder = get_impl().get();
    spv::Block* block = builder.getBuildPoint();
    if (block) {
        builder.createBranch(reinterpret_cast<spv::Block*>(target));
    }
}

void SpirvBuilder::branch_conditional(u32 condition, u32 true_label, u32 false_label) {
    auto& builder = get_impl().get();
    builder.createConditionalBranch(
        static_cast<spv::Id>(condition),
        reinterpret_cast<spv::Block*>(true_label),
        reinterpret_cast<spv::Block*>(false_label)
    );
}

void SpirvBuilder::loop_merge(u32 merge_block, u32 continue_target, u32 control) {
    auto& builder = get_impl().get();
    builder.createLoopMerge(
        reinterpret_cast<spv::Block*>(merge_block),
        reinterpret_cast<spv::Block*>(continue_target),
        static_cast<spv::LoopControlMask>(control),
        {}
    );
}

void SpirvBuilder::selection_merge(u32 merge_block, u32 control) {
    auto& builder = get_impl().get();
    builder.createSelectionMerge(
        reinterpret_cast<spv::Block*>(merge_block),
        static_cast<spv::SelectionControlMask>(control)
    );
}

void SpirvBuilder::kill() {
    auto& builder = get_impl().get();
    builder.makeStatementTerminator(spv::OpKill, "kill");
}

u32 SpirvBuilder::phi(u32 type, const std::vector<std::pair<u32, u32>>& incoming) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> ops;
    for (const auto& [value, block] : incoming) {
        ops.push_back(static_cast<spv::Id>(value));
        ops.push_back(static_cast<spv::Id>(block));
    }
    spv::Id id = builder.createOp(spv::OpPhi, static_cast<spv::Id>(type), ops);
    return static_cast<u32>(id);
}

// Texture operations
u32 SpirvBuilder::sampled_image(u32 type, u32 image, u32 sampler) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createBinOp(spv::OpSampledImage, static_cast<spv::Id>(type),
                                      static_cast<spv::Id>(image), static_cast<spv::Id>(sampler));
    return static_cast<u32>(id);
}

u32 SpirvBuilder::image_sample(u32 type, u32 sampled_img, u32 coord, u32 bias) {
    auto& builder = get_impl().get();
    spv::Builder::TextureParameters params = {};
    params.sampler = static_cast<spv::Id>(sampled_img);
    params.coords = static_cast<spv::Id>(coord);
    if (bias != 0) {
        params.bias = static_cast<spv::Id>(bias);
    }
    spv::Id id = builder.createTextureCall(spv::NoPrecision, static_cast<spv::Id>(type), false, false, false, false, false, params, spv::ImageOperandsMaskNone);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::image_sample_lod(u32 type, u32 sampled_img, u32 coord, u32 lod) {
    auto& builder = get_impl().get();
    spv::Builder::TextureParameters params = {};
    params.sampler = static_cast<spv::Id>(sampled_img);
    params.coords = static_cast<spv::Id>(coord);
    params.lod = static_cast<spv::Id>(lod);
    spv::Id id = builder.createTextureCall(spv::NoPrecision, static_cast<spv::Id>(type), false, false, false, false, false, params, spv::ImageOperandsMaskNone);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::image_sample_grad(u32 type, u32 sampled_img, u32 coord, u32 ddx, u32 ddy) {
    auto& builder = get_impl().get();
    spv::Builder::TextureParameters params = {};
    params.sampler = static_cast<spv::Id>(sampled_img);
    params.coords = static_cast<spv::Id>(coord);
    params.gradX = static_cast<spv::Id>(ddx);
    params.gradY = static_cast<spv::Id>(ddy);
    spv::Id id = builder.createTextureCall(spv::NoPrecision, static_cast<spv::Id>(type), false, false, false, false, false, params, spv::ImageOperandsMaskNone);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::image_fetch(u32 type, u32 image, u32 coord, u32 lod) {
    auto& builder = get_impl().get();
    spv::Builder::TextureParameters params = {};
    params.sampler = static_cast<spv::Id>(image);
    params.coords = static_cast<spv::Id>(coord);
    if (lod != 0) {
        params.lod = static_cast<spv::Id>(lod);
    }
    spv::Id id = builder.createTextureCall(spv::NoPrecision, static_cast<spv::Id>(type), false, false, false, true, false, params, spv::ImageOperandsMaskNone);
    return static_cast<u32>(id);
}

u32 SpirvBuilder::image_query_size_lod(u32 type, u32 image, u32 lod) {
    auto& builder = get_impl().get();
    spv::Id id = builder.createTextureQueryCall(spv::OpImageQuerySizeLod,
                                                 spv::Builder::TextureParameters{},
                                                 false);
    return static_cast<u32>(id);
}

// Decorations
void SpirvBuilder::decorate(u32 target, u32 decoration, const std::vector<u32>& operands) {
    auto& builder = get_impl().get();
    if (operands.empty()) {
        builder.addDecoration(static_cast<spv::Id>(target), static_cast<spv::Decoration>(decoration));
    } else {
        builder.addDecoration(static_cast<spv::Id>(target), static_cast<spv::Decoration>(decoration), operands[0]);
    }
}

void SpirvBuilder::member_decorate(u32 type, u32 member, u32 decoration, const std::vector<u32>& operands) {
    auto& builder = get_impl().get();
    if (operands.empty()) {
        builder.addMemberDecoration(static_cast<spv::Id>(type), member, static_cast<spv::Decoration>(decoration));
    } else {
        builder.addMemberDecoration(static_cast<spv::Id>(type), member, static_cast<spv::Decoration>(decoration), operands[0]);
    }
}

void SpirvBuilder::decorate_array_stride(u32 type, u32 stride) {
    decorate(type, Decoration::ArrayStride, {stride});
}

// Debug
void SpirvBuilder::name(u32 target, const std::string& n) {
    auto& builder = get_impl().get();
    builder.addName(static_cast<spv::Id>(target), n.c_str());
}

void SpirvBuilder::member_name(u32 type, u32 member, const std::string& n) {
    auto& builder = get_impl().get();
    builder.addMemberName(static_cast<spv::Id>(type), member, n.c_str());
}

// Entry point
void SpirvBuilder::entry_point(u32 execution_model, u32 entry_point_id, const std::string& n,
                               const std::vector<u32>& interface_ids) {
    auto& builder = get_impl().get();
    std::vector<spv::Id> ids;
    for (u32 i : interface_ids) {
        ids.push_back(static_cast<spv::Id>(i));
    }
    builder.addEntryPoint(static_cast<spv::ExecutionModel>(execution_model),
                          builder.getMainFunction(),
                          n.c_str());
}

void SpirvBuilder::execution_mode(u32 entry_point_id, u32 mode, const std::vector<u32>& operands) {
    auto& builder = get_impl().get();
    builder.addExecutionMode(builder.getMainFunction(), static_cast<spv::ExecutionMode>(mode));
}

// Extensions
u32 SpirvBuilder::import_extension(const std::string& n) {
    auto& builder = get_impl().get();
    spv::Id id = builder.import(n.c_str());
    return static_cast<u32>(id);
}

void SpirvBuilder::capability(u32 cap) {
    auto& builder = get_impl().get();
    builder.addCapability(static_cast<spv::Capability>(cap));
}

void SpirvBuilder::memory_model(u32 addressing, u32 memory) {
    auto& builder = get_impl().get();
    builder.setMemoryModel(static_cast<spv::AddressingModel>(addressing),
                           static_cast<spv::MemoryModel>(memory));
}

} // namespace x360mu
