/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Shader Cache Implementation
 * Per-game disk caching with LRU eviction and VkPipelineCache persistence.
 */

#include "shader_cache.h"
#include "vulkan/vulkan_backend.h"
#include "xenos/shader_translator.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-shadercache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[SHADERCACHE] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[SHADERCACHE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGW(...) fprintf(stderr, "[SHADERCACHE WARN] " __VA_ARGS__); fprintf(stderr, "\n")
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
    base_cache_path_ = cache_path;
    game_cache_path_ = cache_path;  // Default: no per-game directory until set_title_id

    if (!cache_path.empty()) {
        ensure_directory(cache_path);
        load_cache();
    }

    LOGI("Shader cache initialized (base: %s)", cache_path.c_str());
    return Status::Ok;
}

void ShaderCache::set_title_id(u32 title_id) {
    if (title_id == title_id_ && title_id != 0) return;

    // Save current game's cache before switching
    if (!game_cache_path_.empty() && title_id_ != 0) {
        save_cache();
    }

    // Clear in-memory caches (shaders from previous game)
    if (vulkan_) {
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
    }

    disk_index_.clear();
    lru_order_.clear();
    total_disk_size_ = 0;

    title_id_ = title_id;

    if (title_id != 0 && !base_cache_path_.empty()) {
        // Create per-game directory: base_cache_path/XXXXXXXX/
        std::ostringstream ss;
        ss << base_cache_path_ << "/" << std::hex << std::setfill('0') << std::setw(8) << title_id;
        game_cache_path_ = ss.str();
        ensure_directory(game_cache_path_);

        // Load this game's cache
        load_cache();

        LOGI("Switched to game cache: 0x%08X (%s)", title_id, game_cache_path_.c_str());
    }
}

void ShaderCache::shutdown() {
    if (vulkan_ == nullptr) return;

    VkDevice device = vulkan_->device();

    // Save cache before cleanup
    if (!game_cache_path_.empty()) {
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

    disk_index_.clear();
    lru_order_.clear();

    vulkan_ = nullptr;
    translator_ = nullptr;

    LOGI("Shader cache shutdown (compiled %llu, disk_hits %llu, evicted %llu)",
         (unsigned long long)stats_.shader_compilations,
         (unsigned long long)stats_.shader_disk_hits,
         (unsigned long long)stats_.shaders_evicted);
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

    // Check in-memory cache first
    {
        std::lock_guard<std::mutex> lock(shader_mutex_);
        auto it = shader_cache_.find(hash);
        if (it != shader_cache_.end()) {
            stats_.shader_cache_hits++;
            touch_lru(hash);
            return &it->second;
        }
    }

    // Check disk cache
    if (!game_cache_path_.empty()) {
        CachedShader disk_shader;
        if (load_shader_from_disk(hash, disk_shader)) {
            std::lock_guard<std::mutex> lock(shader_mutex_);
            auto [it, inserted] = shader_cache_.emplace(hash, std::move(disk_shader));
            stats_.shader_disk_hits++;
            touch_lru(hash);
            LOGD("Loaded %s shader from disk: hash=%016llx",
                 type == ShaderType::Vertex ? "vertex" : "pixel",
                 (unsigned long long)hash);
            return &it->second;
        }
    }

    // Translate shader to SPIR-V
    std::vector<u32> spirv;
    try {
        spirv = translator_->translate(microcode, size, type);
    } catch (...) {
        LOGW("Exception translating %s shader (hash=%016llx)",
             type == ShaderType::Vertex ? "vertex" : "pixel",
             (unsigned long long)hash);
        return nullptr;
    }
    if (spirv.empty()) {
        LOGW("Failed to translate %s shader (hash=%016llx)",
             type == ShaderType::Vertex ? "vertex" : "pixel",
             (unsigned long long)hash);
        return nullptr;
    }

    // Create Vulkan shader module
    VkShaderModule module = create_shader_module(spirv);
    if (module == VK_NULL_HANDLE) {
        LOGW("Failed to create %s shader module (hash=%016llx)",
             type == ShaderType::Vertex ? "vertex" : "pixel",
             (unsigned long long)hash);
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

    cached.info.uses_memexport = info.uses_memexport;

    // Save to disk cache
    if (!game_cache_path_.empty()) {
        save_shader_to_disk(cached);
    }

    // Insert into memory cache
    std::lock_guard<std::mutex> lock(shader_mutex_);
    auto [it, inserted] = shader_cache_.emplace(hash, std::move(cached));

    stats_.shader_compilations++;
    touch_lru(hash);

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
    cached.layout = VK_NULL_HANDLE;

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
    state.vertex_input = key.vertex_input;

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

    disk_index_.clear();
    lru_order_.clear();
    total_disk_size_ = 0;

    LOGI("Shader cache cleared");
}

// ============================================================================
// Disk Cache: Index file (shader_index.bin)
// ============================================================================
//
// Format:
//   [4] magic "SIDX"
//   [4] version (SHADER_CACHE_VERSION)
//   [4] entry count
//   For each entry:
//     [8] hash
//     [8] disk_size
//     [8] last_access_time

void ShaderCache::save_cache() {
    if (game_cache_path_.empty()) return;

    // Save shader index
    save_index();

    // Save VkPipelineCache to disk
    std::string pipeline_path = game_cache_path_ + "/pipeline_cache.bin";
    vulkan_->save_pipeline_cache(pipeline_path);

    LOGI("Saved cache: %u shaders, %llu bytes on disk",
         (unsigned)disk_index_.size(), (unsigned long long)total_disk_size_);
}

void ShaderCache::load_cache() {
    if (game_cache_path_.empty()) return;

    // Load shader index
    load_index();

    // Load VkPipelineCache from disk
    std::string pipeline_path = game_cache_path_ + "/pipeline_cache.bin";
    vulkan_->load_pipeline_cache(pipeline_path);
}

void ShaderCache::save_index() {
    std::string path = game_cache_path_ + "/shader_index.bin";
    std::ofstream file(path, std::ios::binary);
    if (!file) return;

    u32 magic = 0x53494458;  // "SIDX"
    u32 version = SHADER_CACHE_VERSION;
    u32 count = static_cast<u32>(disk_index_.size());

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& [hash, entry] : disk_index_) {
        file.write(reinterpret_cast<const char*>(&entry.hash), sizeof(u64));
        file.write(reinterpret_cast<const char*>(&entry.disk_size), sizeof(u64));
        file.write(reinterpret_cast<const char*>(&entry.last_access_time), sizeof(u64));
    }
}

void ShaderCache::load_index() {
    std::string path = game_cache_path_ + "/shader_index.bin";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;

    u32 magic, version, count;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != 0x53494458) {
        LOGW("Invalid shader index magic");
        return;
    }

    if (version != SHADER_CACHE_VERSION) {
        LOGW("Shader cache version mismatch (file=%u, expected=%u) - invalidating",
             version, SHADER_CACHE_VERSION);
        return;
    }

    disk_index_.clear();
    lru_order_.clear();
    total_disk_size_ = 0;

    // Collect entries with timestamps for sorting into LRU order
    struct IndexEntry { u64 hash; u64 disk_size; u64 time; };
    std::vector<IndexEntry> entries;
    entries.reserve(count);

    for (u32 i = 0; i < count && file; i++) {
        DiskCacheEntry entry;
        file.read(reinterpret_cast<char*>(&entry.hash), sizeof(u64));
        file.read(reinterpret_cast<char*>(&entry.disk_size), sizeof(u64));
        file.read(reinterpret_cast<char*>(&entry.last_access_time), sizeof(u64));

        // Verify the .spv file actually exists
        std::string spv_path = shader_file_path(entry.hash);
        struct stat st;
        if (stat(spv_path.c_str(), &st) != 0) {
            continue;  // File missing, skip
        }

        entries.push_back({entry.hash, entry.disk_size, entry.last_access_time});
        disk_index_[entry.hash] = entry;
        total_disk_size_ += entry.disk_size;
    }

    // Sort by access time ascending (oldest first) so LRU front = most recent
    std::sort(entries.begin(), entries.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.time > b.time; });

    for (const auto& e : entries) {
        lru_order_.push_back(e.hash);
    }

    LOGI("Loaded shader index: %zu entries, %llu bytes",
         disk_index_.size(), (unsigned long long)total_disk_size_);
}

// ============================================================================
// Disk Cache: Individual shader files
// ============================================================================
//
// Each shader stored as: game_cache_path/<hash>.spv
// Format:
//   [4] magic "SHDR"
//   [4] version (SHADER_CACHE_VERSION)
//   [4] shader type (ShaderType enum)
//   [4] metadata: texture_bindings
//   [4] metadata: vertex_fetch_bindings
//   [4] metadata: interpolant_mask
//   [1] metadata: uses_memexport
//   [3] reserved
//   [4] spirv word count
//   [N*4] spirv data

std::string ShaderCache::shader_file_path(u64 hash) const {
    std::ostringstream ss;
    ss << game_cache_path_ << "/" << std::hex << std::setfill('0') << std::setw(16) << hash << ".spv";
    return ss.str();
}

bool ShaderCache::save_shader_to_disk(const CachedShader& shader) {
    std::string path = shader_file_path(shader.hash);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        LOGW("Failed to write shader to disk: %s", path.c_str());
        return false;
    }

    u32 magic = 0x53484452;  // "SHDR"
    u32 version = SHADER_CACHE_VERSION;
    u32 type = static_cast<u32>(shader.type);
    u32 spirv_count = static_cast<u32>(shader.spirv.size());
    u8 uses_memexport = shader.info.uses_memexport ? 1 : 0;
    u8 reserved[3] = {0, 0, 0};

    file.write(reinterpret_cast<const char*>(&magic), 4);
    file.write(reinterpret_cast<const char*>(&version), 4);
    file.write(reinterpret_cast<const char*>(&type), 4);
    file.write(reinterpret_cast<const char*>(&shader.texture_bindings), 4);
    file.write(reinterpret_cast<const char*>(&shader.vertex_fetch_bindings), 4);
    file.write(reinterpret_cast<const char*>(&shader.interpolant_mask), 4);
    file.write(reinterpret_cast<const char*>(&uses_memexport), 1);
    file.write(reinterpret_cast<const char*>(reserved), 3);
    file.write(reinterpret_cast<const char*>(&spirv_count), 4);
    file.write(reinterpret_cast<const char*>(shader.spirv.data()), spirv_count * sizeof(u32));

    u64 file_size = 28 + spirv_count * sizeof(u32);

    // Update disk index
    DiskCacheEntry entry;
    entry.hash = shader.hash;
    entry.disk_size = file_size;
    entry.last_access_time = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    disk_index_[shader.hash] = entry;
    total_disk_size_ += file_size;

    // LRU: add to front
    lru_order_.push_front(shader.hash);

    // Evict if over size limit
    if (total_disk_size_ > max_cache_size_) {
        evict_lru();
    }

    return true;
}

bool ShaderCache::load_shader_from_disk(u64 hash, CachedShader& out) {
    auto idx_it = disk_index_.find(hash);
    if (idx_it == disk_index_.end()) return false;

    std::string path = shader_file_path(hash);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // File disappeared - remove from index
        total_disk_size_ -= idx_it->second.disk_size;
        disk_index_.erase(idx_it);
        return false;
    }

    u32 magic, version, type;
    u32 texture_bindings, vertex_fetch_bindings, interpolant_mask;
    u8 uses_memexport;
    u8 reserved[3];
    u32 spirv_count;

    file.read(reinterpret_cast<char*>(&magic), 4);
    file.read(reinterpret_cast<char*>(&version), 4);
    file.read(reinterpret_cast<char*>(&type), 4);
    file.read(reinterpret_cast<char*>(&texture_bindings), 4);
    file.read(reinterpret_cast<char*>(&vertex_fetch_bindings), 4);
    file.read(reinterpret_cast<char*>(&interpolant_mask), 4);
    file.read(reinterpret_cast<char*>(&uses_memexport), 1);
    file.read(reinterpret_cast<char*>(reserved), 3);
    file.read(reinterpret_cast<char*>(&spirv_count), 4);

    if (magic != 0x53484452 || version != SHADER_CACHE_VERSION) {
        LOGW("Stale shader file: %s (version=%u)", path.c_str(), version);
        // Remove stale file
        total_disk_size_ -= idx_it->second.disk_size;
        disk_index_.erase(idx_it);
        return false;
    }

    std::vector<u32> spirv(spirv_count);
    file.read(reinterpret_cast<char*>(spirv.data()), spirv_count * sizeof(u32));

    if (!file) {
        LOGW("Truncated shader file: %s", path.c_str());
        return false;
    }

    // Create VkShaderModule from cached SPIR-V
    VkShaderModule module = create_shader_module(spirv);
    if (module == VK_NULL_HANDLE) {
        LOGW("Failed to create shader module from cached SPIR-V: %s", path.c_str());
        return false;
    }

    out.hash = hash;
    out.type = static_cast<ShaderType>(type);
    out.module = module;
    out.spirv = std::move(spirv);
    out.texture_bindings = texture_bindings;
    out.vertex_fetch_bindings = vertex_fetch_bindings;
    out.interpolant_mask = interpolant_mask;
    out.uses_textures = texture_bindings != 0;
    out.uses_vertex_fetch = vertex_fetch_bindings != 0;
    out.info.uses_memexport = uses_memexport != 0;

    return true;
}

// ============================================================================
// LRU eviction
// ============================================================================

void ShaderCache::touch_lru(u64 hash) {
    // Move to front of LRU list
    lru_order_.remove(hash);
    lru_order_.push_front(hash);

    // Update access time in disk index
    auto it = disk_index_.find(hash);
    if (it != disk_index_.end()) {
        it->second.last_access_time = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
}

void ShaderCache::evict_lru() {
    while (total_disk_size_ > max_cache_size_ && !lru_order_.empty()) {
        u64 victim_hash = lru_order_.back();
        lru_order_.pop_back();

        auto idx_it = disk_index_.find(victim_hash);
        if (idx_it != disk_index_.end()) {
            total_disk_size_ -= idx_it->second.disk_size;
            disk_index_.erase(idx_it);
        }

        // Delete the .spv file
        std::string path = shader_file_path(victim_hash);
        std::remove(path.c_str());

        // Also remove from memory cache if present
        auto mem_it = shader_cache_.find(victim_hash);
        if (mem_it != shader_cache_.end()) {
            if (mem_it->second.module != VK_NULL_HANDLE && vulkan_) {
                vkDestroyShaderModule(vulkan_->device(), mem_it->second.module, nullptr);
            }
            shader_cache_.erase(mem_it);
        }

        stats_.shaders_evicted++;

        LOGD("Evicted shader %016llx (disk size now %llu)",
             (unsigned long long)victim_hash, (unsigned long long)total_disk_size_);
    }
}

// ============================================================================
// Utilities
// ============================================================================

void ShaderCache::ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        mkdir(path.c_str(), 0755);
    }
}

} // namespace x360mu
