/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Memory subsystem - manages guest memory and MMIO
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>

namespace x360mu {

namespace memory {
    constexpr u32 MEM_PAGE_SIZE = 4096;
    constexpr u32 MEM_PAGE_SHIFT = 12;
}

// MMIO handler types
using MmioReadHandler = std::function<u32(GuestAddr addr)>;
using MmioWriteHandler = std::function<void(GuestAddr addr, u32 value)>;

/**
 * Memory region descriptor
 */
struct MemoryRegion {
    GuestAddr base;
    u64 size;
    u32 flags;
    void* host_ptr;
    
    enum Flags : u32 {
        Read    = 1 << 0,
        Write   = 1 << 1,
        Execute = 1 << 2,
        Mmio    = 1 << 3,
    };
};

/**
 * Page table entry (simplified)
 */
struct PageEntry {
    u64 physical_addr;
    u32 flags;
    bool valid;
};

/**
 * Memory management class
 * 
 * Handles:
 * - Guest memory allocation (512MB main RAM)
 * - Fastmem via mmap and signal handling
 * - MMIO dispatch for GPU/audio/peripherals
 * - Page table emulation
 */
class Memory {
public:
    Memory();
    ~Memory();
    
    /**
     * Initialize memory subsystem
     */
    Status initialize();
    
    /**
     * Shutdown and release all memory
     */
    void shutdown();
    
    /**
     * Reset memory to initial state
     */
    void reset();
    
    // ----- Raw memory access (with byte swapping for big-endian) -----
    
    u8 read_u8(GuestAddr addr);
    u16 read_u16(GuestAddr addr);
    u32 read_u32(GuestAddr addr);
    u64 read_u64(GuestAddr addr);
    
    void write_u8(GuestAddr addr, u8 value);
    void write_u16(GuestAddr addr, u16 value);
    void write_u32(GuestAddr addr, u32 value);
    void write_u64(GuestAddr addr, u64 value);
    
    // ----- Address translation -----
    
    GuestAddr translate_address(GuestAddr addr);
    
    // ----- Bulk memory operations -----
    
    void read_bytes(GuestAddr addr, void* dest, u64 size);
    void write_bytes(GuestAddr addr, const void* src, u64 size);
    void zero_bytes(GuestAddr addr, u64 size);
    void copy_bytes(GuestAddr dest, GuestAddr src, u64 size);
    
    // ----- Host pointer access (for DMA, etc.) -----
    
    /**
     * Get host pointer for guest address
     * Returns nullptr if address is MMIO or invalid
     */
    void* get_host_ptr(GuestAddr addr);
    const void* get_host_ptr(GuestAddr addr) const;
    
    /**
     * Translate guest address to physical address
     */
    PhysAddr translate(GuestAddr addr) const;
    
    // ----- Memory allocation (for guest OS emulation) -----
    
    /**
     * Allocate guest memory region
     */
    Status allocate(GuestAddr base, u64 size, u32 flags);
    
    /**
     * Free guest memory region
     */
    void free(GuestAddr base);
    
    /**
     * Protect memory region
     */
    Status protect(GuestAddr base, u64 size, u32 flags);
    
    /**
     * Query memory region info
     */
    bool query(GuestAddr addr, MemoryRegion& out) const;
    
    // ----- MMIO registration -----
    
    /**
     * Register MMIO handlers for address range
     */
    void register_mmio(GuestAddr base, u64 size,
                       MmioReadHandler read,
                       MmioWriteHandler write);
    
    /**
     * Unregister MMIO handlers
     */
    void unregister_mmio(GuestAddr base);
    
    // ----- Write tracking (for GPU texture invalidation) -----
    
    using WriteCallback = std::function<void(GuestAddr addr, u64 size)>;
    
    /**
     * Enable write tracking for address range
     */
    void track_writes(GuestAddr base, u64 size, WriteCallback callback);
    
    /**
     * Disable write tracking
     */
    void untrack_writes(GuestAddr base);
    
    // ----- Atomic/reservation support -----
    
    /**
     * Set reservation for lwarx/ldarx
     */
    void set_reservation(GuestAddr addr, u32 size);
    
    /**
     * Check reservation for stwcx./stdcx.
     */
    bool check_reservation(GuestAddr addr, u32 size) const;
    
    /**
     * Clear reservation
     */
    void clear_reservation();
    
    // ----- Time base -----
    
    /**
     * Get current time base value (64-bit counter)
     */
    u64 get_time_base() const;
    
    /**
     * Increment time base by cycles
     */
    void advance_time_base(u64 cycles);
    
    // ----- Fastmem support -----
    
    /**
     * Get base address for fastmem region
     * Used by JIT for direct memory access
     */
    void* get_fastmem_base() const { return fastmem_base_; }
    
    /**
     * Handle fastmem fault (called from signal handler)
     * Returns true if fault was handled (page mapped)
     * Note: MMIO addresses should use read_u32/write_u32, not fastmem
     */
    bool handle_fault(void* fault_addr);
    
private:
    // Main RAM backing (512MB)
    void* main_memory_ = nullptr;
    u64 main_memory_size_ = 0;
    
    // Fastmem mapping
    void* fastmem_base_ = nullptr;
    u64 fastmem_size_ = 0;
    
    // Page table (simplified - just tracks allocations)
    std::vector<PageEntry> page_table_;
    
    // MMIO handlers
    struct MmioRange {
        GuestAddr base;
        u64 size;
        MmioReadHandler read;
        MmioWriteHandler write;
    };
    std::vector<MmioRange> mmio_handlers_;
    
    // Write tracking
    struct WriteTrack {
        GuestAddr base;
        u64 size;
        WriteCallback callback;
    };
    std::vector<WriteTrack> write_tracks_;
    
    // Memory regions (for query)
    std::vector<MemoryRegion> regions_;
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // Reservation for atomic ops
    GuestAddr reservation_addr_ = 0;
    u32 reservation_size_ = 0;
    bool has_reservation_ = false;
    
    // Time base counter
    std::atomic<u64> time_base_{0};
    
    // Internal helpers
    bool is_mmio(GuestAddr addr) const;
    MmioRange* find_mmio(GuestAddr addr);
    void notify_write(GuestAddr addr, u64 size);
    
    // Fastmem setup
    Status setup_fastmem();
    void teardown_fastmem();
};

} // namespace x360mu

