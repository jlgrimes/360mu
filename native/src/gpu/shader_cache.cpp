/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Shader Cache Implementation
 */

#include "shader_cache.h"
#include "vulkan/vulkan_backend.h"
#include "xenos/shader_translator.h"
#include <fstream>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-shadercache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[SHADERCACHE] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[SHADERCACHE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache() {
    shutdown();
}

Status ShaderCache::initialize(VulkanBackend* vulkan, ShaderTranslator* translator,
                               const std::string& cache_path) {
    vulkan_ = vulkan;
    translator_ = translator;
    cache_path_ = cache_path;
    
    // Load existing cache if available
    if (!cache_path.empty()) {
        load_cache();
    }
    
    LOGI("Shader cache initialized");
    return Status::Ok;
}

void ShaderCache::shutdown() {
    if (vulkan_ == nullptr) return;
    
    VkDevice device = vulkan_->device();
    
    // Save cache before cleanup
    if (!cache_path_.empty()) {
        save_cache();
    }
    
    // Destroy all cached shader modules
    for (auto& [hash, shader] : shader_cache_) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, shader.module, nullptr);
        }
    }
    shader_cache_.clear();
    
    // Destroy all cached pipelines
    for (auto& [hash, pipeline] : pipeline_cache_) {
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        }
    }
    pipeline_cache_.clear();
    
    vulkan_ = nullptr;
    translator_ = nullptr;
    
    LOGI("Shader cache shutdown (compiled %llu shaders, %llu pipelines)",
         stats_.shader_compilations, stats_.pipeline_creations);
}

u64 ShaderCache::compute_microcode_hash(const void* data, u32 size) {
    // FNV-1a hash
    u64 hash = 14695981039346656037ULL;
    const u8* bytes = static_cast<const u8*>(data);
    for (u32 i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

const CachedShader* ShaderCache::get_shader(const void* microcode, u32 size, ShaderType type) {
    if (!vulkan_ || !translator_ || !microcode || size == 0) {
        return nullptr;
    }
    
    u64 hash = compute_microcode_hash(microcode, size);
    
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(shader_mutex_);
        auto it = shader_cache_.find(hash);
        if (it != shader_cache_.end()) {
            stats_.shader_cache_hits++;
            return &it->second;
        }
    }
    
    // Translate shader to SPIR-V
    std::vector<u32> spirv = translator_->translate(microcode, size, type);
    if (spirv.empty()) {
        LOGE("Failed to translate %s shader", 
             type == ShaderType::Vertex ? "vertex" : "pixel");
        return nullptr;
    }
    
    // Create Vulkan shader module
    VkShaderModule module = create_shader_module(spirv);
    if (module == VK_NULL_HANDLE) {
        LOGE("Failed to create shader module");
        return nullptr;
    }
    
    // Get shader info for metadata
    ShaderInfo info = translator_->analyze(microcode, size, type);
    
    // Create cache entry
    CachedShader cached;
    cached.hash = hash;
    cached.type = type;
    cached.module = module;
    cached.spirv = std::move(spirv);
    cached.uses_textures = !info.texture_bindings.empty();
    cached.uses_vertex_fetch = !info.vertex_fetch_slots.empty();
    
    // Build bitmasks
    cached.texture_bindings = 0;
    for (u32 binding : info.texture_bindings) {
        if (binding < 32) cached.texture_bindings |= (1u << binding);
    }
    
    cached.vertex_fetch_bindings = 0;
    for (u32 slot : info.vertex_fetch_slots) {
        if (slot < 32) cached.vertex_fetch_bindings |= (1u << slot);
    }
    
    cached.interpolant_mask = 0;
    for (const auto& interp : info.interpolants) {
        if (interp.index < 16) cached.interpolant_mask |= (1u << interp.index);
    }
    
    // Insert into cache
    std::lock_guard<std::mutex> lock(shader_mutex_);
    auto [it, inserted] = shader_cache_.emplace(hash, std::move(cached));
    
    stats_.shader_compilations++;
    
    LOGD("Compiled %s shader: hash=%016llx, %zu SPIR-V words",
         type == ShaderType::Vertex ? "vertex" : "pixel",
         (unsigned long long)hash, it->second.spirv.size());
    
    return &it->second;
}

VkShaderModule ShaderCache::create_shader_module(const std::vector<u32>& spirv) {
    return vulkan_->create_shader_module(spirv);
}

VkPipeline ShaderCache::get_pipeline(const CachedShader* vertex_shader,
                                     const CachedShader* pixel_shader,
                                     const PipelineKey& key) {
    if (!vulkan_ || !vertex_shader || !pixel_shader) {
        return VK_NULL_HANDLE;
    }
    
    u64 key_hash = key.compute_hash();
    
    // Check cache
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        auto it = pipeline_cache_.find(key_hash);
        if (it != pipeline_cache_.end()) {
            stats_.pipeline_cache_hits++;
            return it->second.pipeline;
        }
    }
    
    // Create new pipeline
    VkPipeline pipeline = create_graphics_pipeline(vertex_shader, pixel_shader, key);
    if (pipeline == VK_NULL_HANDLE) {
        LOGE("Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }
    
    // Cache it
    CachedPipeline cached;
    cached.key = key;
    cached.pipeline = pipeline;
    cached.layout = VK_NULL_HANDLE;  // Use default layout from backend
    
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_cache_[key_hash] = cached;
    
    stats_.pipeline_creations++;
    
    LOGD("Created graphics pipeline: vs=%016llx, ps=%016llx",
         (unsigned long long)key.vertex_shader_hash,
         (unsigned long long)key.pixel_shader_hash);
    
    return pipeline;
}

VkPipeline ShaderCache::create_graphics_pipeline(const CachedShader* vs,
                                                  const CachedShader* ps,
                                                  const PipelineKey& key) {
    // Create pipeline state from key
    PipelineState state;
    state.primitive_topology = key.primitive_topology;
    state.cull_mode = key.cull_mode;
    state.front_face = key.front_face;
    state.depth_test_enable = key.depth_test_enable;
    state.depth_write_enable = key.depth_write_enable;
    state.depth_compare_op = key.depth_compare_op;
    state.blend_enable = key.blend_enable;
    state.src_color_blend = key.src_color_blend;
    state.dst_color_blend = key.dst_color_blend;
    state.color_blend_op = key.color_blend_op;
    
    return vulkan_->get_or_create_pipeline(state, vs->module, ps->module);
}

void ShaderCache::clear() {
    std::lock_guard<std::mutex> lock1(shader_mutex_);
    std::lock_guard<std::mutex> lock2(pipeline_mutex_);
    
    VkDevice device = vulkan_->device();
    
    for (auto& [hash, shader] : shader_cache_) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, shader.module, nullptr);
        }
    }
    shader_cache_.clear();
    
    for (auto& [hash, pipeline] : pipeline_cache_) {
        if (pipeline.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        }
    }
    pipeline_cache_.clear();
    
    LOGI("Shader cache cleared");
}

void ShaderCache::save_cache() {
    if (cache_path_.empty()) return;
    
    std::string path = cache_path_ + "/shader_modules.bin";
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    
    // Write header
    u32 magic = 0x53484452;  // "SHDR"
    u32 version = 1;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // Write shader count
    u32 count = static_cast<u32>(shader_cache_.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    
    // Write each shader's SPIR-V (not the VkShaderModule, which isn't portable)
    for (const auto& [hash, shader] : shader_cache_) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        u32 type = static_cast<u32>(shader.type);
        file.write(reinterpret_cast<const char*>(&type), sizeof(type));
        u32 spirv_size = static_cast<u32>(shader.spirv.size());
        file.write(reinterpret_cast<const char*>(&spirv_size), sizeof(spirv_size));
        file.write(reinterpret_cast<const char*>(shader.spirv.data()), 
                   spirv_size * sizeof(u32));
        
        // Write metadata
        file.write(reinterpret_cast<const char*>(&shader.texture_bindings), sizeof(u32));
        file.write(reinterpret_cast<const char*>(&shader.vertex_fetch_bindings), sizeof(u32));
        file.write(reinterpret_cast<const char*>(&shader.interpolant_mask), sizeof(u32));
    }
    
    LOGI("Saved %u shaders to cache", count);
}

void ShaderCache::load_cache() {
    if (cache_path_.empty()) return;
    
    std::string path = cache_path_ + "/shader_modules.bin";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    
    // Read header
    u32 magic, version;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    
    if (magic != 0x53484452 || version != 1) {
        LOGE("Invalid shader cache file");
        return;
    }
    
    // Read shader count
    u32 count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    
    u32 loaded = 0;
    for (u32 i = 0; i < count && file; i++) {
        u64 hash;
        u32 type;
        u32 spirv_size;
        
        file.read(reinterpret_cast<char*>(&hash), sizeof(hash));
        file.read(reinterpret_cast<char*>(&type), sizeof(type));
        file.read(reinterpret_cast<char*>(&spirv_size), sizeof(spirv_size));
        
        std::vector<u32> spirv(spirv_size);
        file.read(reinterpret_cast<char*>(spirv.data()), spirv_size * sizeof(u32));
        
        u32 texture_bindings, vertex_fetch_bindings, interpolant_mask;
        file.read(reinterpret_cast<char*>(&texture_bindings), sizeof(u32));
        file.read(reinterpret_cast<char*>(&vertex_fetch_bindings), sizeof(u32));
        file.read(reinterpret_cast<char*>(&interpolant_mask), sizeof(u32));
        
        // Create shader module from cached SPIR-V
        VkShaderModule module = create_shader_module(spirv);
        if (module == VK_NULL_HANDLE) {
            continue;
        }
        
        CachedShader cached;
        cached.hash = hash;
        cached.type = static_cast<ShaderType>(type);
        cached.module = module;
        cached.spirv = std::move(spirv);
        cached.texture_bindings = texture_bindings;
        cached.vertex_fetch_bindings = vertex_fetch_bindings;
        cached.interpolant_mask = interpolant_mask;
        cached.uses_textures = texture_bindings != 0;
        cached.uses_vertex_fetch = vertex_fetch_bindings != 0;
        
        shader_cache_[hash] = std::move(cached);
        loaded++;
    }
    
    LOGI("Loaded %u/%u shaders from cache", loaded, count);
}

} // namespace x360mu
