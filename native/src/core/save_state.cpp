/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Save State Implementation
 */

#include "save_state.h"
#include "cpu/xenon/cpu.h"
#include "gpu/xenos/gpu.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include <cstring>
#include <ctime>
#include <algorithm>

// zlib is available in Android NDK and most systems
#ifdef __ANDROID__
#include <zlib.h>
#else
// Fallback: no compression on non-Android builds
#define NO_ZLIB
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-save"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[SAVE] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[SAVE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// File I/O helpers
//=============================================================================

bool SaveState::write_all(FILE* f, const void* data, u64 size) {
    return fwrite(data, 1, size, f) == size;
}

bool SaveState::read_all(FILE* f, void* data, u64 size) {
    return fread(data, 1, size, f) == size;
}

//=============================================================================
// Checksum
//=============================================================================

u64 SaveState::fnv1a(const u8* data, u64 size) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (u64 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

//=============================================================================
// Compression
//=============================================================================

std::vector<u8> SaveState::compress(const u8* data, u64 size) {
#ifndef NO_ZLIB
    uLongf bound = compressBound(static_cast<uLong>(size));
    std::vector<u8> out(bound);

    int ret = compress2(out.data(), &bound, data, static_cast<uLong>(size), Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) {
        LOGE("zlib compress failed: %d", ret);
        return {};
    }
    out.resize(bound);
    return out;
#else
    // No compression — return copy
    return std::vector<u8>(data, data + size);
#endif
}

std::vector<u8> SaveState::decompress(const u8* data, u64 compressed_size, u64 uncompressed_size) {
#ifndef NO_ZLIB
    std::vector<u8> out(uncompressed_size);
    uLongf dest_len = static_cast<uLongf>(uncompressed_size);

    int ret = uncompress(out.data(), &dest_len, data, static_cast<uLong>(compressed_size));
    if (ret != Z_OK) {
        LOGE("zlib decompress failed: %d", ret);
        return {};
    }
    out.resize(dest_len);
    return out;
#else
    return std::vector<u8>(data, data + compressed_size);
#endif
}

//=============================================================================
// CPU serialization
//=============================================================================

std::vector<u8> SaveState::serialize_cpu(Cpu* cpu) {
    CpuStateBlob blob{};

    for (u32 t = 0; t < 6; t++) {
        const ThreadContext& ctx = cpu->get_context(t);
        auto& th = blob.threads[t];

        // GPRs
        for (u32 i = 0; i < 32; i++) th.gpr[i] = ctx.gpr[i];

        // FPRs — store bit pattern
        for (u32 i = 0; i < 32; i++) {
            memcpy(&th.fpr_bits[i], &ctx.fpr[i], sizeof(u64));
        }

        // Vector regs
        for (u32 i = 0; i < 128; i++) {
            th.vr[i][0] = ctx.vr[i].u64x2[0];
            th.vr[i][1] = ctx.vr[i].u64x2[1];
        }

        th.lr = ctx.lr;
        th.ctr = ctx.ctr;
        th.xer = ctx.xer.to_u32();

        for (u32 i = 0; i < 8; i++) th.cr[i] = ctx.cr[i].to_byte();

        th.fpscr = ctx.fpscr;
        th.vscr = ctx.vscr;
        th.pc = ctx.pc;
        th.msr = ctx.msr;
        th.time_base = ctx.time_base;
        th.thread_id = ctx.thread_id;
        th.running = ctx.running ? 1 : 0;
        th.interrupted = ctx.interrupted ? 1 : 0;
        th.has_reservation = ctx.has_reservation ? 1 : 0;
        th.reservation_addr = ctx.reservation_addr;
        th.reservation_size = ctx.reservation_size;
    }

    const u8* ptr = reinterpret_cast<const u8*>(&blob);
    return std::vector<u8>(ptr, ptr + sizeof(blob));
}

Status SaveState::deserialize_cpu(const u8* data, u64 size, Cpu* cpu) {
    if (size < sizeof(CpuStateBlob)) return Status::InvalidFormat;

    const CpuStateBlob& blob = *reinterpret_cast<const CpuStateBlob*>(data);

    for (u32 t = 0; t < 6; t++) {
        ThreadContext& ctx = cpu->get_context(t);
        const auto& th = blob.threads[t];

        for (u32 i = 0; i < 32; i++) ctx.gpr[i] = th.gpr[i];
        for (u32 i = 0; i < 32; i++) memcpy(&ctx.fpr[i], &th.fpr_bits[i], sizeof(u64));

        for (u32 i = 0; i < 128; i++) {
            ctx.vr[i].u64x2[0] = th.vr[i][0];
            ctx.vr[i].u64x2[1] = th.vr[i][1];
        }

        ctx.lr = th.lr;
        ctx.ctr = th.ctr;
        ctx.xer.from_u32(th.xer);
        for (u32 i = 0; i < 8; i++) ctx.cr[i].from_byte(th.cr[i]);

        ctx.fpscr = th.fpscr;
        ctx.vscr = th.vscr;
        ctx.pc = th.pc;
        ctx.msr = th.msr;
        ctx.time_base = th.time_base;
        ctx.thread_id = th.thread_id;
        ctx.running = th.running != 0;
        ctx.interrupted = th.interrupted != 0;
        ctx.has_reservation = th.has_reservation != 0;
        ctx.reservation_addr = th.reservation_addr;
        ctx.reservation_size = th.reservation_size;
    }

    return Status::Ok;
}

//=============================================================================
// GPU serialization
//=============================================================================

std::vector<u8> SaveState::serialize_gpu(Gpu* gpu) {
    // Save GPU register file + ring buffer state
    constexpr u32 REG_COUNT = 0x10000;

    u64 total = sizeof(GpuStateBlob) + REG_COUNT * sizeof(u32);
    std::vector<u8> out(total);

    GpuStateBlob* hdr = reinterpret_cast<GpuStateBlob*>(out.data());
    hdr->register_count = REG_COUNT;
    hdr->ring_buffer_base = 0;  // Will be restored from registers
    hdr->ring_buffer_size = 0;
    hdr->read_ptr = 0;
    hdr->write_ptr = 0;

    // Dump register file
    u32* regs = reinterpret_cast<u32*>(out.data() + sizeof(GpuStateBlob));
    for (u32 i = 0; i < REG_COUNT; i++) {
        regs[i] = gpu->read_register(i);
    }

    return out;
}

Status SaveState::deserialize_gpu(const u8* data, u64 size, Gpu* gpu) {
    if (size < sizeof(GpuStateBlob)) return Status::InvalidFormat;

    const GpuStateBlob* hdr = reinterpret_cast<const GpuStateBlob*>(data);
    u32 reg_count = hdr->register_count;

    if (size < sizeof(GpuStateBlob) + reg_count * sizeof(u32)) {
        return Status::InvalidFormat;
    }

    const u32* regs = reinterpret_cast<const u32*>(data + sizeof(GpuStateBlob));
    for (u32 i = 0; i < reg_count; i++) {
        gpu->write_register(i, regs[i]);
    }

    return Status::Ok;
}

//=============================================================================
// Kernel serialization
//=============================================================================

// Helper: write a length-prefixed string to buffer
static void write_string(std::vector<u8>& buf, const std::string& s) {
    u32 len = static_cast<u32>(s.size());
    buf.insert(buf.end(), reinterpret_cast<const u8*>(&len),
               reinterpret_cast<const u8*>(&len) + sizeof(len));
    buf.insert(buf.end(), s.begin(), s.end());
}

// Helper: read a length-prefixed string from buffer
static const u8* read_string(const u8* p, const u8* end, std::string& out) {
    if (p + sizeof(u32) > end) return nullptr;
    u32 len;
    memcpy(&len, p, sizeof(u32));
    p += sizeof(u32);
    if (p + len > end) return nullptr;
    out.assign(reinterpret_cast<const char*>(p), len);
    return p + len;
}

std::vector<u8> SaveState::serialize_kernel(Kernel* kernel) {
    std::vector<u8> buf;

    // Reserve space for header
    KernelStateBlob hdr{};
    buf.resize(sizeof(KernelStateBlob));

    // Serialize modules
    // We can't directly access private members, so we use the public API
    // For now, serialize what we can access through public methods
    // Module info is needed to know what was loaded
    hdr.module_count = 0;
    hdr.thread_count = 0;
    hdr.object_count = 0;
    hdr.next_handle = 0;

    // Write header
    memcpy(buf.data(), &hdr, sizeof(hdr));

    return buf;
}

Status SaveState::deserialize_kernel(const u8* data, u64 size, Kernel* kernel) {
    if (size < sizeof(KernelStateBlob)) return Status::InvalidFormat;

    // Kernel state restoration is limited by public API
    // The kernel will be re-initialized from the loaded game
    // Thread states are restored via CPU context
    (void)kernel;

    return Status::Ok;
}

//=============================================================================
// Memory serialization (with compression)
//=============================================================================

std::vector<u8> SaveState::serialize_memory(Memory* memory) {
    constexpr u64 TOTAL_SIZE = 512 * MB;
    constexpr u32 PAGE_SIZE = 4096;
    constexpr u32 PAGE_COUNT = TOTAL_SIZE / PAGE_SIZE;
    constexpr u32 BITMAP_SIZE = PAGE_COUNT / 8;

    // Build page bitmap — only save non-zero pages
    std::vector<u8> bitmap(BITMAP_SIZE, 0);
    std::vector<u8> page_data;
    u32 dirty_count = 0;

    const u8* base = static_cast<const u8*>(memory->get_host_ptr(0));
    if (!base) {
        LOGE("Cannot get memory base pointer");
        return {};
    }

    for (u32 page = 0; page < PAGE_COUNT; page++) {
        const u8* page_ptr = base + page * PAGE_SIZE;

        // Check if page has any non-zero data
        bool has_data = false;
        const u64* qwords = reinterpret_cast<const u64*>(page_ptr);
        for (u32 i = 0; i < PAGE_SIZE / sizeof(u64); i++) {
            if (qwords[i] != 0) {
                has_data = true;
                break;
            }
        }

        if (has_data) {
            bitmap[page / 8] |= (1 << (page % 8));
            page_data.insert(page_data.end(), page_ptr, page_ptr + PAGE_SIZE);
            dirty_count++;
        }
    }

    LOGI("Memory: %u/%u pages dirty (%.1f MB of %.0f MB)",
         dirty_count, PAGE_COUNT,
         dirty_count * PAGE_SIZE / (1024.0 * 1024.0),
         TOTAL_SIZE / (1024.0 * 1024.0));

    // Compress page data
    std::vector<u8> compressed = compress(page_data.data(), page_data.size());
    bool is_compressed = !compressed.empty() && compressed.size() < page_data.size();

    if (!is_compressed) {
        compressed = std::move(page_data);
    }

    LOGI("Memory compressed: %zu → %zu bytes (%.1f%%)",
         (size_t)(dirty_count * PAGE_SIZE),
         compressed.size(),
         compressed.size() * 100.0 / std::max((u64)1, (u64)dirty_count * PAGE_SIZE));

    // Build output: header + bitmap + compressed data
    MemoryStateHeader hdr{};
    hdr.total_size = TOTAL_SIZE;
    hdr.page_size = PAGE_SIZE;
    hdr.page_count = PAGE_COUNT;
    hdr.dirty_page_count = dirty_count;
    hdr.compressed = is_compressed ? 1 : 0;

    std::vector<u8> out;
    out.reserve(sizeof(hdr) + BITMAP_SIZE + compressed.size());

    out.insert(out.end(), reinterpret_cast<const u8*>(&hdr),
               reinterpret_cast<const u8*>(&hdr) + sizeof(hdr));
    out.insert(out.end(), bitmap.begin(), bitmap.end());
    out.insert(out.end(), compressed.begin(), compressed.end());

    return out;
}

Status SaveState::deserialize_memory(const u8* data, u64 size, Memory* memory) {
    if (size < sizeof(MemoryStateHeader)) return Status::InvalidFormat;

    MemoryStateHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.page_size != 4096 || hdr.total_size != 512 * MB) {
        LOGE("Invalid memory state header");
        return Status::InvalidFormat;
    }

    u32 bitmap_size = hdr.page_count / 8;
    if (size < sizeof(hdr) + bitmap_size) return Status::InvalidFormat;

    const u8* bitmap = data + sizeof(hdr);
    const u8* compressed_data = bitmap + bitmap_size;
    u64 compressed_size = size - sizeof(hdr) - bitmap_size;

    // Decompress page data
    u64 uncompressed_size = (u64)hdr.dirty_page_count * hdr.page_size;
    std::vector<u8> page_data;

    if (hdr.compressed) {
        page_data = decompress(compressed_data, compressed_size, uncompressed_size);
        if (page_data.empty()) {
            LOGE("Failed to decompress memory state");
            return Status::InvalidFormat;
        }
    } else {
        page_data.assign(compressed_data, compressed_data + compressed_size);
    }

    // Zero all memory first
    u8* base = static_cast<u8*>(memory->get_host_ptr(0));
    if (!base) return Status::Error;
    memset(base, 0, hdr.total_size);

    // Restore dirty pages
    u32 page_idx = 0;
    for (u32 page = 0; page < hdr.page_count; page++) {
        if (bitmap[page / 8] & (1 << (page % 8))) {
            if (page_idx * hdr.page_size + hdr.page_size > page_data.size()) {
                LOGE("Page data underflow at page %u", page);
                return Status::InvalidFormat;
            }
            memcpy(base + page * hdr.page_size,
                   page_data.data() + page_idx * hdr.page_size,
                   hdr.page_size);
            page_idx++;
        }
    }

    LOGI("Memory restored: %u pages", hdr.dirty_page_count);
    return Status::Ok;
}

//=============================================================================
// Main save/load
//=============================================================================

Status SaveState::save(const std::string& path,
                       Cpu* cpu, Gpu* gpu, Memory* memory, Kernel* kernel) {
    LOGI("Saving state to: %s", path.c_str());

    if (!cpu || !gpu || !memory) {
        LOGE("Missing subsystem for save state");
        return Status::InvalidArgument;
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGE("Failed to open save file: %s", path.c_str());
        return Status::IoError;
    }

    // Serialize each section
    auto cpu_data = serialize_cpu(cpu);
    auto gpu_data = serialize_gpu(gpu);
    auto kernel_data = kernel ? serialize_kernel(kernel) : std::vector<u8>();
    auto memory_data = serialize_memory(memory);

    if (memory_data.empty()) {
        LOGE("Failed to serialize memory");
        fclose(f);
        return Status::Error;
    }

    // Build section list
    struct Section {
        SaveSection type;
        std::vector<u8>& data;
    };
    Section sections[] = {
        {SaveSection::Cpu,    cpu_data},
        {SaveSection::Gpu,    gpu_data},
        {SaveSection::Kernel, kernel_data},
        {SaveSection::Memory, memory_data},
    };

    // Compute checksum over all section data
    u64 checksum = 0xcbf29ce484222325ULL;
    for (auto& s : sections) {
        if (!s.data.empty()) {
            u64 section_hash = fnv1a(s.data.data(), s.data.size());
            checksum ^= section_hash;
            checksum *= 0x100000001b3ULL;
        }
    }

    // Write file header
    SaveStateHeader header{};
    header.magic = SAVE_STATE_MAGIC;
    header.version = SAVE_STATE_VERSION;
    header.section_count = 4;
    header.flags = 0;
    header.timestamp = static_cast<u64>(time(nullptr));
    header.checksum = checksum;

    if (!write_all(f, &header, sizeof(header))) {
        LOGE("Failed to write header");
        fclose(f);
        return Status::IoError;
    }

    // Write sections
    for (auto& s : sections) {
        SectionHeader sh{};
        sh.type = static_cast<u32>(s.type);
        sh.flags = 0;
        sh.uncompressed_size = s.data.size();
        sh.compressed_size = s.data.size();

        if (!write_all(f, &sh, sizeof(sh))) {
            fclose(f);
            return Status::IoError;
        }
        if (!s.data.empty() && !write_all(f, s.data.data(), s.data.size())) {
            fclose(f);
            return Status::IoError;
        }
    }

    fclose(f);

    LOGI("State saved: %zu + %zu + %zu + %zu bytes",
         cpu_data.size(), gpu_data.size(), kernel_data.size(), memory_data.size());
    return Status::Ok;
}

Status SaveState::load(const std::string& path,
                       Cpu* cpu, Gpu* gpu, Memory* memory, Kernel* kernel) {
    LOGI("Loading state from: %s", path.c_str());

    if (!cpu || !gpu || !memory) {
        LOGE("Missing subsystem for load state");
        return Status::InvalidArgument;
    }

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOGE("Failed to open save file: %s", path.c_str());
        return Status::IoError;
    }

    // Read file header
    SaveStateHeader header;
    if (!read_all(f, &header, sizeof(header))) {
        LOGE("Failed to read header");
        fclose(f);
        return Status::InvalidFormat;
    }

    if (header.magic != SAVE_STATE_MAGIC) {
        LOGE("Invalid save state magic: 0x%08X", header.magic);
        fclose(f);
        return Status::InvalidFormat;
    }

    if (header.version != SAVE_STATE_VERSION) {
        LOGE("Incompatible save state version: %u (expected %u)",
             header.version, SAVE_STATE_VERSION);
        fclose(f);
        return Status::InvalidFormat;
    }

    // Read sections
    for (u32 i = 0; i < header.section_count; i++) {
        SectionHeader sh;
        if (!read_all(f, &sh, sizeof(sh))) {
            LOGE("Failed to read section header %u", i);
            fclose(f);
            return Status::InvalidFormat;
        }

        std::vector<u8> data(sh.compressed_size);
        if (sh.compressed_size > 0 && !read_all(f, data.data(), sh.compressed_size)) {
            LOGE("Failed to read section data %u (%llu bytes)", i, sh.compressed_size);
            fclose(f);
            return Status::InvalidFormat;
        }

        // Dispatch to appropriate deserializer
        Status status = Status::Ok;
        SaveSection type = static_cast<SaveSection>(sh.type);

        switch (type) {
            case SaveSection::Cpu:
                status = deserialize_cpu(data.data(), data.size(), cpu);
                break;
            case SaveSection::Gpu:
                status = deserialize_gpu(data.data(), data.size(), gpu);
                break;
            case SaveSection::Kernel:
                if (kernel) {
                    status = deserialize_kernel(data.data(), data.size(), kernel);
                }
                break;
            case SaveSection::Memory:
                status = deserialize_memory(data.data(), data.size(), memory);
                break;
            default:
                LOGI("Skipping unknown section type: 0x%08X", sh.type);
                break;
        }

        if (status != Status::Ok) {
            LOGE("Failed to deserialize section %u (type=0x%08X)", i, sh.type);
            fclose(f);
            return status;
        }
    }

    fclose(f);
    LOGI("State loaded successfully");
    return Status::Ok;
}

} // namespace x360mu
