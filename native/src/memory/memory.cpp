/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Memory subsystem implementation
 */

#include "memory.h"
#include <cstring>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-mem"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[MEM] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[MEM ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// Global memory instance for signal handler
static Memory* g_memory_instance = nullptr;

// Signal handler for fastmem faults
static void fastmem_signal_handler(int sig, siginfo_t* info, void* context) {
    if (g_memory_instance && g_memory_instance->handle_fault(info->si_addr, true)) {
        return; // Fault handled
    }
    
    // Not our fault, re-raise
    signal(sig, SIG_DFL);
    raise(sig);
}

Memory::Memory() = default;

Memory::~Memory() {
    shutdown();
}

Status Memory::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOGI("Initializing memory subsystem");
    
    // Allocate main memory (512MB)
    main_memory_size_ = memory::MAIN_MEMORY_SIZE;
    main_memory_ = mmap(
        nullptr,
        main_memory_size_,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
    
    if (main_memory_ == MAP_FAILED) {
        LOGE("Failed to allocate main memory");
        return Status::OutOfMemory;
    }
    
    LOGI("Allocated %llu MB main memory at %p", 
         main_memory_size_ / MB, main_memory_);
    
    // Initialize page table
    u64 page_count = main_memory_size_ / memory::PAGE_SIZE;
    page_table_.resize(page_count);
    for (u64 i = 0; i < page_count; i++) {
        page_table_[i].physical_addr = i * memory::PAGE_SIZE;
        page_table_[i].flags = MemoryRegion::Read | MemoryRegion::Write;
        page_table_[i].valid = true;
    }
    
    // Setup fastmem
    Status status = setup_fastmem();
    if (status != Status::Ok) {
        LOGE("Fastmem setup failed, using slow path");
        // Non-fatal, continue without fastmem
    }
    
    LOGI("Memory subsystem initialized");
    return Status::Ok;
}

void Memory::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    teardown_fastmem();
    
    if (main_memory_ && main_memory_ != MAP_FAILED) {
        munmap(main_memory_, main_memory_size_);
        main_memory_ = nullptr;
    }
    
    page_table_.clear();
    regions_.clear();
    mmio_handlers_.clear();
    write_tracks_.clear();
}

void Memory::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Zero out main memory
    if (main_memory_) {
        memset(main_memory_, 0, main_memory_size_);
    }
    
    // Reset page table flags (keep valid)
    for (auto& page : page_table_) {
        page.flags = MemoryRegion::Read | MemoryRegion::Write;
    }
}

Status Memory::setup_fastmem() {
    // Reserve a large virtual address space for fastmem
    // This allows direct address translation via pointer arithmetic
    fastmem_size_ = 4ULL * GB; // 4GB address space
    
    fastmem_base_ = mmap(
        nullptr,
        fastmem_size_,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
        -1, 0
    );
    
    if (fastmem_base_ == MAP_FAILED) {
        LOGE("Failed to reserve fastmem address space");
        fastmem_base_ = nullptr;
        return Status::OutOfMemory;
    }
    
    LOGI("Reserved fastmem at %p (4GB)", fastmem_base_);
    
    // Map main memory into the fastmem region
    void* mapped = mmap(
        fastmem_base_,
        main_memory_size_,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
        -1, 0
    );
    
    if (mapped == MAP_FAILED) {
        LOGE("Failed to map main memory into fastmem");
        munmap(fastmem_base_, fastmem_size_);
        fastmem_base_ = nullptr;
        return Status::Error;
    }
    
    // Copy initial memory content
    memcpy(fastmem_base_, main_memory_, main_memory_size_);
    
    // Install signal handler for page faults
    g_memory_instance = this;
    
    struct sigaction sa;
    sa.sa_sigaction = fastmem_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
        LOGE("Failed to install SIGSEGV handler");
    }
    if (sigaction(SIGBUS, &sa, nullptr) != 0) {
        LOGE("Failed to install SIGBUS handler");
    }
    
    LOGI("Fastmem initialized successfully");
    return Status::Ok;
}

void Memory::teardown_fastmem() {
    if (fastmem_base_) {
        munmap(fastmem_base_, fastmem_size_);
        fastmem_base_ = nullptr;
        g_memory_instance = nullptr;
    }
}

bool Memory::handle_fault(void* fault_addr, bool is_write) {
    if (!fastmem_base_) return false;
    
    uintptr_t addr = reinterpret_cast<uintptr_t>(fault_addr);
    uintptr_t base = reinterpret_cast<uintptr_t>(fastmem_base_);
    
    // Check if fault is in our fastmem region
    if (addr < base || addr >= base + fastmem_size_) {
        return false;
    }
    
    GuestAddr guest_addr = static_cast<GuestAddr>(addr - base);
    
    // Check for MMIO
    if (is_mmio(guest_addr)) {
        // MMIO faults need special handling
        // For now, just log and return false (will crash)
        LOGE("MMIO fault at 0x%08X (not implemented in fastmem)", guest_addr);
        return false;
    }
    
    // Check if this is a valid address that just needs mapping
    if (guest_addr < main_memory_size_) {
        // Map the page
        void* page_addr = reinterpret_cast<void*>(
            base + align_down(guest_addr, static_cast<GuestAddr>(memory::PAGE_SIZE))
        );
        
        void* mapped = mmap(
            page_addr,
            memory::PAGE_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1, 0
        );
        
        if (mapped != MAP_FAILED) {
            return true;
        }
    }
    
    return false;
}

// Memory access with byte swapping (Xbox 360 is big-endian)
u8 Memory::read_u8(GuestAddr addr) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            return static_cast<u8>(handler->read(addr));
        }
        return 0;
    }
    
    if (addr >= main_memory_size_) return 0;
    return static_cast<u8*>(main_memory_)[addr];
}

u16 Memory::read_u16(GuestAddr addr) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            return static_cast<u16>(handler->read(addr));
        }
        return 0;
    }
    
    if (addr + 1 >= main_memory_size_) return 0;
    u16 value;
    memcpy(&value, static_cast<u8*>(main_memory_) + addr, sizeof(u16));
    return byte_swap(value); // Big-endian to host
}

u32 Memory::read_u32(GuestAddr addr) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            return handler->read(addr);
        }
        return 0;
    }
    
    if (addr + 3 >= main_memory_size_) return 0;
    u32 value;
    memcpy(&value, static_cast<u8*>(main_memory_) + addr, sizeof(u32));
    return byte_swap(value);
}

u64 Memory::read_u64(GuestAddr addr) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            u64 lo = handler->read(addr);
            u64 hi = handler->read(addr + 4);
            return (hi << 32) | lo;
        }
        return 0;
    }
    
    if (addr + 7 >= main_memory_size_) return 0;
    u64 value;
    memcpy(&value, static_cast<u8*>(main_memory_) + addr, sizeof(u64));
    return byte_swap(value);
}

void Memory::write_u8(GuestAddr addr, u8 value) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            handler->write(addr, value);
        }
        return;
    }
    
    if (addr >= main_memory_size_) return;
    static_cast<u8*>(main_memory_)[addr] = value;
    notify_write(addr, 1);
}

void Memory::write_u16(GuestAddr addr, u16 value) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            handler->write(addr, byte_swap(value));
        }
        return;
    }
    
    if (addr + 1 >= main_memory_size_) return;
    value = byte_swap(value); // Host to big-endian
    memcpy(static_cast<u8*>(main_memory_) + addr, &value, sizeof(u16));
    notify_write(addr, 2);
}

void Memory::write_u32(GuestAddr addr, u32 value) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            handler->write(addr, value);
        }
        return;
    }
    
    if (addr + 3 >= main_memory_size_) return;
    value = byte_swap(value);
    memcpy(static_cast<u8*>(main_memory_) + addr, &value, sizeof(u32));
    notify_write(addr, 4);
}

void Memory::write_u64(GuestAddr addr, u64 value) {
    if (is_mmio(addr)) {
        auto* handler = find_mmio(addr);
        if (handler) {
            handler->write(addr, static_cast<u32>(value >> 32));
            handler->write(addr + 4, static_cast<u32>(value));
        }
        return;
    }
    
    if (addr + 7 >= main_memory_size_) return;
    value = byte_swap(value);
    memcpy(static_cast<u8*>(main_memory_) + addr, &value, sizeof(u64));
    notify_write(addr, 8);
}

void Memory::read_bytes(GuestAddr addr, void* dest, u64 size) {
    if (addr + size > main_memory_size_) {
        size = main_memory_size_ - addr;
    }
    memcpy(dest, static_cast<u8*>(main_memory_) + addr, size);
}

void Memory::write_bytes(GuestAddr addr, const void* src, u64 size) {
    if (addr + size > main_memory_size_) {
        size = main_memory_size_ - addr;
    }
    memcpy(static_cast<u8*>(main_memory_) + addr, src, size);
    notify_write(addr, size);
}

void Memory::zero_bytes(GuestAddr addr, u64 size) {
    if (addr + size > main_memory_size_) {
        size = main_memory_size_ - addr;
    }
    memset(static_cast<u8*>(main_memory_) + addr, 0, size);
    notify_write(addr, size);
}

void Memory::copy_bytes(GuestAddr dest, GuestAddr src, u64 size) {
    if (src + size > main_memory_size_ || dest + size > main_memory_size_) {
        return;
    }
    memmove(
        static_cast<u8*>(main_memory_) + dest,
        static_cast<u8*>(main_memory_) + src,
        size
    );
    notify_write(dest, size);
}

void* Memory::get_host_ptr(GuestAddr addr) {
    if (is_mmio(addr)) return nullptr;
    if (addr >= main_memory_size_) return nullptr;
    return static_cast<u8*>(main_memory_) + addr;
}

const void* Memory::get_host_ptr(GuestAddr addr) const {
    if (is_mmio(addr)) return nullptr;
    if (addr >= main_memory_size_) return nullptr;
    return static_cast<const u8*>(main_memory_) + addr;
}

PhysAddr Memory::translate(GuestAddr addr) const {
    // Simple identity mapping for now
    // Real implementation would use page tables
    if (addr >= memory::PHYSICAL_BASE && addr <= memory::PHYSICAL_END) {
        return addr - memory::PHYSICAL_BASE;
    }
    return addr;
}

Status Memory::allocate(GuestAddr base, u64 size, u32 flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check alignment
    if (!is_aligned(base, static_cast<GuestAddr>(memory::PAGE_SIZE))) {
        return Status::InvalidArgument;
    }
    
    // Check bounds
    if (base + size > main_memory_size_) {
        return Status::OutOfMemory;
    }
    
    // Update page table
    u64 start_page = base / memory::PAGE_SIZE;
    u64 page_count = (size + memory::PAGE_SIZE - 1) / memory::PAGE_SIZE;
    
    for (u64 i = 0; i < page_count; i++) {
        page_table_[start_page + i].flags = flags;
        page_table_[start_page + i].valid = true;
    }
    
    // Add to regions
    regions_.push_back({
        .base = base,
        .size = size,
        .flags = flags,
        .host_ptr = static_cast<u8*>(main_memory_) + base
    });
    
    return Status::Ok;
}

void Memory::free(GuestAddr base) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Find and remove region
    for (auto it = regions_.begin(); it != regions_.end(); ++it) {
        if (it->base == base) {
            // Clear page table entries
            u64 start_page = base / memory::PAGE_SIZE;
            u64 page_count = (it->size + memory::PAGE_SIZE - 1) / memory::PAGE_SIZE;
            
            for (u64 i = 0; i < page_count; i++) {
                page_table_[start_page + i].flags = 0;
                page_table_[start_page + i].valid = false;
            }
            
            regions_.erase(it);
            return;
        }
    }
}

Status Memory::protect(GuestAddr base, u64 size, u32 flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    u64 start_page = base / memory::PAGE_SIZE;
    u64 page_count = (size + memory::PAGE_SIZE - 1) / memory::PAGE_SIZE;
    
    for (u64 i = 0; i < page_count; i++) {
        if (start_page + i < page_table_.size()) {
            page_table_[start_page + i].flags = flags;
        }
    }
    
    return Status::Ok;
}

bool Memory::query(GuestAddr addr, MemoryRegion& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& region : regions_) {
        if (addr >= region.base && addr < region.base + region.size) {
            out = region;
            return true;
        }
    }
    return false;
}

void Memory::register_mmio(GuestAddr base, u64 size,
                           MmioReadHandler read,
                           MmioWriteHandler write) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    mmio_handlers_.push_back({
        .base = base,
        .size = size,
        .read = std::move(read),
        .write = std::move(write)
    });
    
    LOGI("Registered MMIO: 0x%08X - 0x%08X", base, base + static_cast<u32>(size));
}

void Memory::unregister_mmio(GuestAddr base) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto it = mmio_handlers_.begin(); it != mmio_handlers_.end(); ++it) {
        if (it->base == base) {
            mmio_handlers_.erase(it);
            return;
        }
    }
}

bool Memory::is_mmio(GuestAddr addr) const {
    // GPU registers
    if (addr >= memory::GPU_REGS_BASE && addr <= memory::GPU_REGS_END) {
        return true;
    }
    
    // Check registered MMIO ranges
    for (const auto& handler : mmio_handlers_) {
        if (addr >= handler.base && addr < handler.base + handler.size) {
            return true;
        }
    }
    
    return false;
}

Memory::MmioRange* Memory::find_mmio(GuestAddr addr) {
    for (auto& handler : mmio_handlers_) {
        if (addr >= handler.base && addr < handler.base + handler.size) {
            return &handler;
        }
    }
    return nullptr;
}

void Memory::track_writes(GuestAddr base, u64 size, WriteCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    write_tracks_.push_back({
        .base = base,
        .size = size,
        .callback = std::move(callback)
    });
}

void Memory::untrack_writes(GuestAddr base) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto it = write_tracks_.begin(); it != write_tracks_.end(); ++it) {
        if (it->base == base) {
            write_tracks_.erase(it);
            return;
        }
    }
}

void Memory::notify_write(GuestAddr addr, u64 size) {
    for (const auto& track : write_tracks_) {
        // Check for overlap
        if (addr < track.base + track.size && addr + size > track.base) {
            track.callback(addr, size);
        }
    }
    
    // Clear reservation if write overlaps
    if (has_reservation_) {
        if (addr < reservation_addr_ + reservation_size_ && 
            addr + size > reservation_addr_) {
            has_reservation_ = false;
        }
    }
}

// Atomic reservation support
void Memory::set_reservation(GuestAddr addr, u32 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    reservation_addr_ = addr;
    reservation_size_ = size;
    has_reservation_ = true;
}

bool Memory::check_reservation(GuestAddr addr, u32 size) const {
    if (!has_reservation_) return false;
    return (addr == reservation_addr_ && size == reservation_size_);
}

void Memory::clear_reservation() {
    has_reservation_ = false;
}

// Time base support
u64 Memory::get_time_base() const {
    return time_base_.load(std::memory_order_relaxed);
}

void Memory::advance_time_base(u64 cycles) {
    time_base_.fetch_add(cycles, std::memory_order_relaxed);
}

} // namespace x360mu

