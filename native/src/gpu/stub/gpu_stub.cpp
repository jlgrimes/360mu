/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Stub GPU implementation when Vulkan is not available
 */

// Define empty types for forward declared classes to avoid unique_ptr issues
namespace x360mu {
    class VulkanBackend {};
    class CommandProcessor {};
}

#include "gpu/xenos/gpu.h"
#include "memory/memory.h"

namespace x360mu {

// Gpu implementation
Gpu::Gpu() = default;
Gpu::~Gpu() = default;

Status Gpu::initialize(Memory* memory, const GpuConfig& config) {
    memory_ = memory;
    config_ = config;
    registers_.fill(0);
    return Status::Ok;
}

void Gpu::shutdown() {
    // Nothing to do
}

void Gpu::reset() {
    registers_.fill(0);
    render_state_ = {};
    frame_complete_ = false;
    in_frame_ = false;
    stats_ = {};
}

void Gpu::set_surface(void* /*native_window*/) {}

void Gpu::resize(u32 /*width*/, u32 /*height*/) {}

void Gpu::process_commands() {
    // Stub - no GPU commands processed
}

void Gpu::present() {
    frame_complete_ = false;
    stats_.frames++;
}

u32 Gpu::read_register(u32 offset) {
    if (offset < registers_.size()) {
        return registers_[offset];
    }
    return 0;
}

void Gpu::write_register(u32 offset, u32 value) {
    if (offset < registers_.size()) {
        registers_[offset] = value;
    }
}

void Gpu::execute_packet(u32 /*packet*/) {}
void Gpu::execute_type0(u32 /*packet*/) {}
void Gpu::execute_type3(u32 /*packet*/) {}

void Gpu::cmd_draw_indices(PrimitiveType /*type*/, u32 /*index_count*/, GuestAddr /*index_addr*/) {}
void Gpu::cmd_draw_auto(PrimitiveType /*type*/, u32 /*vertex_count*/) {}
void Gpu::cmd_resolve() {}

void Gpu::update_render_state() {}
void Gpu::update_shaders() {}
void Gpu::update_textures() {}

// ShaderTranslator stub
ShaderTranslator::ShaderTranslator() = default;
ShaderTranslator::~ShaderTranslator() = default;

std::vector<u32> ShaderTranslator::translate(const void* /*microcode*/, u32 /*size*/, ShaderType /*type*/) {
    return {};
}

const std::vector<u32>* ShaderTranslator::get_cached(u64 hash) const {
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        return &it->second;
    }
    return nullptr;
}

void ShaderTranslator::cache(u64 hash, std::vector<u32> spirv) {
    cache_[hash] = std::move(spirv);
}

void ShaderTranslator::decode_alu_instruction(u32 /*instr*/) {}
void ShaderTranslator::decode_fetch_instruction(u32 /*instr*/) {}
void ShaderTranslator::emit_spirv_header(std::vector<u32>& /*out*/) {}
void ShaderTranslator::emit_spirv_body(std::vector<u32>& /*out*/) {}

// TextureCache stub
TextureCache::TextureCache() = default;
TextureCache::~TextureCache() = default;

void* TextureCache::get_texture(const FetchConstant& /*fetch*/, Memory* /*memory*/) {
    return nullptr;
}

void TextureCache::invalidate(GuestAddr /*base*/, u64 /*size*/) {}
void TextureCache::clear() {
    textures_.clear();
}

void TextureCache::detile_texture(const void* /*src*/, void* /*dst*/, u32 /*width*/, u32 /*height*/, TextureFormat /*format*/) {}
void TextureCache::convert_format(const void* /*src*/, void* /*dst*/, u32 /*width*/, u32 /*height*/, TextureFormat /*src_format*/) {}

} // namespace x360mu
