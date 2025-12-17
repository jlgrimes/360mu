/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Core type definitions and constants
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace x360mu {

// Standard integer types
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using f32 = float;
using f64 = double;

// Size types
using usize = std::size_t;
using isize = std::ptrdiff_t;

// Xbox 360 specific types
using GuestAddr = u32;      // 32-bit Xbox virtual address
using HostAddr  = u64;      // 64-bit host address
using PhysAddr  = u32;      // Physical address

// GPU types
using GpuAddr   = u32;      // GPU address
using VertexFetch = u32;    // Vertex fetch constant

// Memory sizes
constexpr u64 KB = 1024ULL;
constexpr u64 MB = 1024ULL * KB;
constexpr u64 GB = 1024ULL * MB;

// Xbox 360 memory map constants
namespace memory {
    // Main memory (512MB)
    constexpr GuestAddr MAIN_MEMORY_BASE = 0x00000000;
    constexpr u64 MAIN_MEMORY_SIZE = 512 * MB;
    
    // Physical memory mapping  
    constexpr GuestAddr PHYSICAL_BASE = 0x80000000;
    constexpr GuestAddr PHYSICAL_END  = 0x8FFFFFFF;
    
    // GPU registers
    constexpr GuestAddr GPU_REGS_BASE = 0x7FC00000;
    constexpr GuestAddr GPU_REGS_END  = 0x7FFFFFFF;
    
    // Command buffer
    constexpr GuestAddr CMD_BUFFER_BASE = 0xC0000000;
    constexpr GuestAddr CMD_BUFFER_END  = 0xFFFFFFFF;
    
    // eDRAM (GPU only, 10MB)
    constexpr u64 EDRAM_SIZE = 10 * MB;
    
    // Page size (4KB)
    constexpr u64 PAGE_SIZE = 4 * KB;
    constexpr u64 PAGE_SHIFT = 12;
    constexpr u64 PAGE_MASK = PAGE_SIZE - 1;
    
    // Large page (64KB)
    constexpr u64 LARGE_PAGE_SIZE = 64 * KB;
    constexpr u64 LARGE_PAGE_SHIFT = 16;
}

// CPU constants
namespace cpu {
    constexpr u32 NUM_GPRS = 32;          // General purpose registers
    constexpr u32 NUM_FPRS = 32;          // Floating point registers
    constexpr u32 NUM_VMX_REGS = 128;     // VMX128 vector registers
    constexpr u32 NUM_CORES = 3;          // Xenon cores
    constexpr u32 THREADS_PER_CORE = 2;   // Hardware threads per core
    constexpr u32 NUM_THREADS = NUM_CORES * THREADS_PER_CORE;
    
    constexpr u64 CLOCK_SPEED = 3200000000ULL; // 3.2 GHz
}

// GPU constants
namespace gpu {
    constexpr u32 SHADER_PROCESSORS = 48;
    constexpr u64 CLOCK_SPEED = 500000000ULL;  // 500 MHz
    constexpr u32 MAX_TEXTURES = 16;
    constexpr u32 MAX_RENDER_TARGETS = 4;
    constexpr u32 MAX_VERTEX_BUFFERS = 16;
}

// Byte order conversion (Xbox 360 is big-endian)
template<typename T>
constexpr T byte_swap(T value) {
    static_assert(std::is_integral_v<T>, "byte_swap requires integral type");
    
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<u16>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<u32>(value)));
    } else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(__builtin_bswap64(static_cast<u64>(value)));
    }
}

// Big-endian value wrapper
template<typename T>
struct be {
    T raw;
    
    be() = default;
    be(T value) : raw(byte_swap(value)) {}
    
    operator T() const { return byte_swap(raw); }
    be& operator=(T value) { raw = byte_swap(value); return *this; }
    
    T get() const { return byte_swap(raw); }
    void set(T value) { raw = byte_swap(value); }
};

// Common big-endian types
using be_u16 = be<u16>;
using be_u32 = be<u32>;
using be_u64 = be<u64>;

// Alignment helpers
template<typename T>
constexpr T align_up(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

template<typename T>
constexpr T align_down(T value, T alignment) {
    return value & ~(alignment - 1);
}

template<typename T>
constexpr bool is_aligned(T value, T alignment) {
    return (value & (alignment - 1)) == 0;
}

// Bit manipulation
template<typename T>
constexpr T bit(unsigned n) {
    return static_cast<T>(1) << n;
}

template<typename T>
constexpr bool test_bit(T value, unsigned n) {
    return (value & bit<T>(n)) != 0;
}

template<typename T>
constexpr T set_bit(T value, unsigned n) {
    return value | bit<T>(n);
}

template<typename T>
constexpr T clear_bit(T value, unsigned n) {
    return value & ~bit<T>(n);
}

template<typename T>
constexpr T extract_bits(T value, unsigned start, unsigned count) {
    return (value >> start) & ((static_cast<T>(1) << count) - 1);
}

// Result type for error handling
enum class Status {
    Ok = 0,
    Error,
    InvalidArgument,
    NotFound,
    NotImplemented,
    OutOfMemory,
    InvalidFormat,
    IoError,
    Timeout,
};

inline const char* status_to_string(Status s) {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::Error: return "Error";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::NotFound: return "NotFound";
        case Status::NotImplemented: return "NotImplemented";
        case Status::OutOfMemory: return "OutOfMemory";
        case Status::InvalidFormat: return "InvalidFormat";
        case Status::IoError: return "IoError";
        case Status::Timeout: return "Timeout";
        default: return "Unknown";
    }
}

} // namespace x360mu

