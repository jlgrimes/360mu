/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Save State Infrastructure
 *
 * Binary format:
 *   FileHeader
 *   SectionHeader + data (CPU)
 *   SectionHeader + data (GPU)
 *   SectionHeader + data (Kernel)
 *   SectionHeader + data (Memory - zlib compressed, dirty pages only)
 *
 * Memory is compressed with zlib deflate. Only non-zero 4KB pages
 * are saved, with a bitmap indicating which pages are present.
 */

#pragma once

#include "x360mu/types.h"
#include <string>
#include <vector>
#include <cstdio>

namespace x360mu {

class Cpu;
class Gpu;
class Memory;
class Kernel;

// File format magic: "360S" (360 Save)
constexpr u32 SAVE_STATE_MAGIC = 0x33363053;
constexpr u32 SAVE_STATE_VERSION = 1;

/**
 * Section types in save state file
 */
enum class SaveSection : u32 {
    Cpu       = 0x43505500,  // "CPU\0"
    Gpu       = 0x47505500,  // "GPU\0"
    Kernel    = 0x4B524E00,  // "KRN\0"
    Memory    = 0x4D454D00,  // "MEM\0"
    EdramData = 0x45445200,  // "EDR\0"
};

/**
 * File header (32 bytes)
 */
struct SaveStateHeader {
    u32 magic;               // SAVE_STATE_MAGIC
    u32 version;             // SAVE_STATE_VERSION
    u32 section_count;       // Number of sections
    u32 flags;               // Reserved flags
    u64 timestamp;           // Unix timestamp
    u64 checksum;            // FNV-1a of all section data
};
static_assert(sizeof(SaveStateHeader) == 32);

/**
 * Section header (24 bytes)
 */
struct SectionHeader {
    u32 type;                // SaveSection enum
    u32 flags;               // Section-specific flags
    u64 uncompressed_size;   // Original data size
    u64 compressed_size;     // Size on disk (== uncompressed if no compression)
};
static_assert(sizeof(SectionHeader) == 24);

/**
 * CPU state blob — serialized per-thread
 */
struct CpuStateBlob {
    // Per thread (6 threads)
    struct Thread {
        u64 gpr[32];
        u64 fpr_bits[32];     // f64 stored as u64 bit pattern
        u64 vr[128][2];       // 128-bit vector as 2x u64
        u64 lr, ctr;
        u32 xer;              // Packed XER
        u8  cr[8];            // CR fields as bytes
        u32 fpscr, vscr;
        u64 pc, msr;
        u64 time_base;
        u32 thread_id;
        u8  running;
        u8  interrupted;
        u8  has_reservation;
        u8  _pad;
        u32 reservation_addr;
        u32 reservation_size;
    };

    Thread threads[6];
};

/**
 * GPU state blob — register file
 */
struct GpuStateBlob {
    u32 register_count;      // Number of registers saved
    u32 ring_buffer_base;
    u32 ring_buffer_size;
    u32 read_ptr;
    u32 write_ptr;
    // Followed by register_count * u32 register values
};

/**
 * Kernel state blob header
 */
struct KernelStateBlob {
    u32 next_handle;
    u32 module_count;
    u32 thread_count;
    u32 object_count;
    // Followed by serialized modules, threads, objects
};

/**
 * Memory page bitmap + compressed data
 */
struct MemoryStateHeader {
    u64 total_size;          // Total guest memory size
    u32 page_size;           // Page size (4096)
    u32 page_count;          // Total pages
    u32 dirty_page_count;    // Pages actually saved
    u32 compressed;          // 1 if zlib compressed
    // Followed by:
    //   - page_count/8 bytes bitmap (1 bit per page, 1=present)
    //   - compressed page data
};

/**
 * Save state serializer/deserializer
 */
class SaveState {
public:
    /**
     * Save entire emulator state to file
     * Emulator must be paused before calling
     */
    static Status save(const std::string& path,
                       Cpu* cpu, Gpu* gpu, Memory* memory, Kernel* kernel);

    /**
     * Load emulator state from file
     * Restores CPU, GPU registers, memory, and kernel state
     */
    static Status load(const std::string& path,
                       Cpu* cpu, Gpu* gpu, Memory* memory, Kernel* kernel);

private:
    // Section serializers
    static std::vector<u8> serialize_cpu(Cpu* cpu);
    static std::vector<u8> serialize_gpu(Gpu* gpu);
    static std::vector<u8> serialize_kernel(Kernel* kernel);
    static std::vector<u8> serialize_memory(Memory* memory);

    // Section deserializers
    static Status deserialize_cpu(const u8* data, u64 size, Cpu* cpu);
    static Status deserialize_gpu(const u8* data, u64 size, Gpu* gpu);
    static Status deserialize_kernel(const u8* data, u64 size, Kernel* kernel);
    static Status deserialize_memory(const u8* data, u64 size, Memory* memory);

    // Compression
    static std::vector<u8> compress(const u8* data, u64 size);
    static std::vector<u8> decompress(const u8* data, u64 compressed_size, u64 uncompressed_size);

    // Checksum
    static u64 fnv1a(const u8* data, u64 size);

    // File I/O helpers
    static bool write_all(FILE* f, const void* data, u64 size);
    static bool read_all(FILE* f, void* data, u64 size);
};

} // namespace x360mu
