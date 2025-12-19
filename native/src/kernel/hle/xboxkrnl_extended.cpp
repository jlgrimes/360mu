/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Extended xboxkrnl.exe HLE functions
 * 
 * This file contains:
 * - Thread creation and management
 * - Physical memory management
 * - Time functions
 * - Interlocked operations
 * - Object management
 * - Exception handling
 * - DPC/APC/Timer support
 * - XEX module functions
 */

#include "../kernel.h"
#include "../xobject.h"
#include "../../cpu/xenon/cpu.h"
#include "../../cpu/xenon/threading.h"
#include "../../memory/memory.h"
#include "../../apu/xma_decoder.h"
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-hle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[HLE-EXT] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[HLE-EXT WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[HLE-EXT ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// NTSTATUS codes
constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr u32 STATUS_NO_MEMORY = 0xC0000017;
constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr u32 STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr u32 STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr u32 STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr u32 STATUS_TIMEOUT = 0x00000102;
constexpr u32 STATUS_INVALID_HANDLE = 0xC0000008;

// Thread creation flags
constexpr u32 CREATE_SUSPENDED = 0x00000004;

//=============================================================================
// Global Extended HLE State
//=============================================================================
static struct ExtendedHleState {
    // Time tracking
    std::chrono::steady_clock::time_point boot_time;
    u64 performance_counter_offset;
    
    // Thread management
    struct ThreadEntry {
        u32 handle;
        u32 thread_id;
        GuestAddr entry_point;
        GuestAddr stack_base;
        u64 stack_size;
        GuestAddr tls_base;
        u32 priority;
        bool suspended;
        bool terminated;
        std::thread* host_thread;
        ThreadContext context;
    };
    std::unordered_map<u32, ThreadEntry> threads;
    std::mutex thread_mutex;
    std::atomic<u32> next_handle{0x80000100};
    std::atomic<u32> next_thread_id{1};
    
    // Current thread per hardware thread (0-5)
    std::array<u32, 6> current_thread_handle;
    
    // TLS management
    std::atomic<u32> next_tls_slot{0};
    std::array<u64, 64> tls_slots;
    
    // Physical memory tracking
    struct PhysAllocation {
        GuestAddr addr;
        u64 size;
        u32 protect;
    };
    std::unordered_map<GuestAddr, PhysAllocation> physical_allocations;
    GuestAddr next_physical_addr = 0xA0000000;
    std::mutex phys_mutex;
    
    // Module tracking
    struct LoadedModule {
        u32 handle;
        std::string name;
        GuestAddr base;
        u64 size;
        GuestAddr entry_point;
    };
    std::vector<LoadedModule> modules;
    u32 next_module_handle = 0x10000;
    
    // DPC queue
    struct DpcEntry {
        GuestAddr routine;
        GuestAddr context;
        GuestAddr arg1;
        GuestAddr arg2;
    };
    std::vector<DpcEntry> dpc_queue;
    std::mutex dpc_mutex;
    
    void init() {
        boot_time = std::chrono::steady_clock::now();
        performance_counter_offset = 0;
        current_thread_handle.fill(0);
        tls_slots.fill(0);
    }
    
} g_ext_hle;

// Scheduler pointer (set by kernel init, accessible from other HLE modules)
ThreadScheduler* g_scheduler = nullptr;

//=============================================================================
// Extended Memory Functions
//=============================================================================

static void HLE_MmAllocatePhysicalMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // PVOID MmAllocatePhysicalMemory(ULONG Flags, SIZE_T Size, ULONG Protect);
    u32 flags = static_cast<u32>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    u32 protect = static_cast<u32>(args[2]);
    
    // Align to page size
    size = align_up(size, static_cast<u32>(memory::MEM_PAGE_SIZE));
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    GuestAddr addr = g_ext_hle.next_physical_addr;
    g_ext_hle.next_physical_addr += size + memory::MEM_PAGE_SIZE;  // Leave gap
    
    // Perform allocation
    u32 mem_flags = MemoryRegion::Read | MemoryRegion::Write;
    if (protect & 0x10) mem_flags |= MemoryRegion::Execute;
    
    Status status = memory->allocate(addr, size, mem_flags);
    
    if (status == Status::Ok) {
        g_ext_hle.physical_allocations[addr] = {addr, size, protect};
        memory->zero_bytes(addr, size);
        *result = addr;
        LOGD("MmAllocatePhysicalMemory: size=0x%X -> 0x%08X", size, addr);
    } else {
        *result = 0;
        LOGW("MmAllocatePhysicalMemory: FAILED size=0x%X", size);
    }
}

static void HLE_MmAllocatePhysicalMemoryEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // PVOID MmAllocatePhysicalMemoryEx(
    //   ULONG Flags,
    //   SIZE_T Size,
    //   ULONG Protect,
    //   ULONG_PTR MinAddress,
    //   ULONG_PTR MaxAddress,
    //   ULONG Alignment
    // );
    u32 flags = static_cast<u32>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    u32 protect = static_cast<u32>(args[2]);
    GuestAddr min_addr = static_cast<GuestAddr>(args[3]);
    GuestAddr max_addr = static_cast<GuestAddr>(args[4]);
    u32 alignment = static_cast<u32>(args[5]);
    
    if (alignment == 0) alignment = memory::MEM_PAGE_SIZE;
    size = align_up(size, alignment);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    // Try to allocate within range
    GuestAddr addr = align_up(
        std::max(g_ext_hle.next_physical_addr, min_addr),
        static_cast<GuestAddr>(alignment)
    );
    
    if (max_addr != 0 && addr + size > max_addr) {
        // Try from min_addr
        addr = align_up(min_addr, static_cast<GuestAddr>(alignment));
    }
    
    g_ext_hle.next_physical_addr = addr + size + memory::MEM_PAGE_SIZE;
    
    u32 mem_flags = MemoryRegion::Read | MemoryRegion::Write;
    if (protect & 0x10) mem_flags |= MemoryRegion::Execute;
    
    Status status = memory->allocate(addr, size, mem_flags);
    
    if (status == Status::Ok) {
        g_ext_hle.physical_allocations[addr] = {addr, size, protect};
        memory->zero_bytes(addr, size);
        *result = addr;
        LOGD("MmAllocatePhysicalMemoryEx: size=0x%X, align=0x%X -> 0x%08X", size, alignment, addr);
    } else {
        *result = 0;
    }
}

static void HLE_MmFreePhysicalMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 type = static_cast<u32>(args[0]);
    GuestAddr addr = static_cast<GuestAddr>(args[1]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    auto it = g_ext_hle.physical_allocations.find(addr);
    if (it != g_ext_hle.physical_allocations.end()) {
        memory->free(addr);
        g_ext_hle.physical_allocations.erase(it);
        LOGD("MmFreePhysicalMemory: freed 0x%08X", addr);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_MmGetPhysicalAddress(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr virtual_addr = static_cast<GuestAddr>(args[0]);
    // Simple identity mapping
    *result = virtual_addr;
}

static void HLE_MmMapIoSpace(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // PVOID MmMapIoSpace(
    //   PHYSICAL_ADDRESS PhysicalAddress,
    //   SIZE_T NumberOfBytes,
    //   MEMORY_CACHING_TYPE CacheType
    // );
    GuestAddr phys_addr = static_cast<GuestAddr>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    u32 cache_type = static_cast<u32>(args[2]);
    
    // For MMIO, just return the physical address as virtual
    // The memory system will handle MMIO dispatch
    *result = phys_addr;
    LOGD("MmMapIoSpace: 0x%08X, size=0x%X", phys_addr, size);
}

static void HLE_MmUnmapIoSpace(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Nothing to do for simple identity mapping
    *result = 0;
}

static void HLE_MmQueryAddressProtect(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    for (const auto& [base, alloc] : g_ext_hle.physical_allocations) {
        if (addr >= base && addr < base + alloc.size) {
            *result = alloc.protect;
            return;
        }
    }
    
    // Default to PAGE_READWRITE
    *result = 0x04;
}

static void HLE_MmSetAddressProtect(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    u32 protect = static_cast<u32>(args[2]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    auto it = g_ext_hle.physical_allocations.find(addr);
    if (it != g_ext_hle.physical_allocations.end()) {
        it->second.protect = protect;
    }
    
    *result = 0;
}

static void HLE_MmQueryAllocationSize(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.phys_mutex);
    
    auto it = g_ext_hle.physical_allocations.find(addr);
    if (it != g_ext_hle.physical_allocations.end()) {
        *result = it->second.size;
    } else {
        MemoryRegion region;
        if (memory->query(addr, region)) {
            *result = region.size;
        } else {
            *result = 0;
        }
    }
}

static void HLE_MmQueryStatistics(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr stats_ptr = static_cast<GuestAddr>(args[0]);
    
    // MM_STATISTICS structure
    memory->write_u32(stats_ptr + 0, sizeof(u32) * 16);  // Length
    memory->write_u32(stats_ptr + 4, 512 * MB);         // TotalPhysicalPages (512MB)
    memory->write_u32(stats_ptr + 8, 256 * MB);         // AvailablePages
    memory->write_u32(stats_ptr + 12, 0);               // SystemCachePages
    memory->write_u32(stats_ptr + 16, 0);               // PoolPages
    memory->write_u32(stats_ptr + 20, 0);               // StackPages
    memory->write_u32(stats_ptr + 24, 0);               // ImagePages
    memory->write_u32(stats_ptr + 28, 0);               // HeapPages
    memory->write_u32(stats_ptr + 32, 0);               // VirtualMappedPages
    memory->write_u32(stats_ptr + 36, memory::MEM_PAGE_SIZE); // PageSize
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Thread and Process Functions
//=============================================================================

static void HLE_ExCreateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS ExCreateThread(
    //   PHANDLE pHandle,          // arg[0]
    //   SIZE_T StackSize,         // arg[1]
    //   PDWORD pThreadId,         // arg[2]
    //   PVOID ApiThreadStartup,   // arg[3] - entry point
    //   PVOID StartRoutine,       // arg[4] - actual start routine
    //   PVOID StartContext,       // arg[5] - parameter
    //   DWORD CreationFlags       // arg[6]
    // );
    
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 stack_size = static_cast<u32>(args[1]);
    GuestAddr thread_id_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr api_entry = static_cast<GuestAddr>(args[3]);
    GuestAddr start_routine = static_cast<GuestAddr>(args[4]);
    GuestAddr start_context = static_cast<GuestAddr>(args[5]);
    u32 creation_flags = static_cast<u32>(args[6]);
    
    // Default stack size
    if (stack_size == 0) stack_size = 64 * KB;
    stack_size = align_up(stack_size, static_cast<u32>(memory::MEM_PAGE_SIZE));
    
    // Allocate stack in virtual address range that maps to physical memory
    // 0x8E000000 maps to physical 0x0E000000 (within 512MB main memory)
    GuestAddr stack_base = 0x8E000000 + (g_ext_hle.threads.size() * (stack_size + memory::MEM_PAGE_SIZE));
    memory->allocate(stack_base & 0x1FFFFFFF, stack_size, MemoryRegion::Read | MemoryRegion::Write);
    
    // Generate IDs
    u32 handle = g_ext_hle.next_handle++;
    u32 thread_id = g_ext_hle.next_thread_id++;
    
    std::lock_guard<std::mutex> lock(g_ext_hle.thread_mutex);
    
    // Create thread entry
    auto& thread = g_ext_hle.threads[handle];
    thread.handle = handle;
    thread.thread_id = thread_id;
    thread.entry_point = api_entry ? api_entry : start_routine;
    thread.stack_base = stack_base;
    thread.stack_size = stack_size;
    thread.priority = 0;
    thread.suspended = (creation_flags & CREATE_SUSPENDED) != 0;
    thread.terminated = false;
    thread.host_thread = nullptr;
    
    // Initialize thread context
    thread.context.reset();
    thread.context.pc = thread.entry_point;
    thread.context.gpr[1] = stack_base + stack_size - 0x80;  // Stack pointer (leave red zone)
    thread.context.gpr[3] = start_context;  // First argument
    thread.context.gpr[4] = start_routine;  // Second argument (if using ApiThreadStartup)
    thread.context.thread_id = thread_id % 6;  // Map to hardware thread
    
    // If using scheduler, create guest thread
    if (g_scheduler) {
        GuestThread* guest_thread = g_scheduler->create_thread(
            thread.entry_point, start_context, stack_size, creation_flags
        );
        if (guest_thread) {
            thread.handle = guest_thread->handle;
        }
    }
    
    // Write output
    memory->write_u32(handle_ptr, handle);
    if (thread_id_ptr) {
        memory->write_u32(thread_id_ptr, thread_id);
    }
    
    LOGI("ExCreateThread: handle=0x%X, id=%u, entry=0x%08X, stack=0x%08X, context=0x%08X",
         handle, thread_id, thread.entry_point, stack_base, start_context);
    
    *result = STATUS_SUCCESS;
}

static void HLE_ExTerminateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 exit_code = static_cast<u32>(args[0]);
    
    // Get current thread
    u32 current_handle = g_ext_hle.current_thread_handle[0];  // Simplified
    
    std::lock_guard<std::mutex> lock(g_ext_hle.thread_mutex);
    
    auto it = g_ext_hle.threads.find(current_handle);
    if (it != g_ext_hle.threads.end()) {
        it->second.terminated = true;
        LOGI("ExTerminateThread: handle=0x%X, exit_code=%u", current_handle, exit_code);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtTerminateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    u32 exit_code = static_cast<u32>(args[1]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.thread_mutex);
    
    auto it = g_ext_hle.threads.find(handle);
    if (it != g_ext_hle.threads.end()) {
        it->second.terminated = true;
        LOGI("NtTerminateThread: handle=0x%X, exit_code=%u", handle, exit_code);
        *result = STATUS_SUCCESS;
    } else {
        *result = STATUS_INVALID_HANDLE;
    }
}

static void HLE_KeGetCurrentThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return KTHREAD pointer for current thread
    // Use GetCurrentGuestThread() to get the actual thread handle
    GuestThread* guest = GetCurrentGuestThread();
    u32 handle = 0x80000001;  // Default fallback
    
    if (guest) {
        handle = guest->handle;
    }
    
    // Return pseudo-KTHREAD pointer
    // KTHREAD is at 0x80070000 + (handle & 0xFFFF) * 0x200
    *result = 0x80070000 + (handle & 0xFFFF) * 0x200;
    
    static int ke_log_count = 0;
    if (ke_log_count++ < 5) {
        LOGI("KeGetCurrentThread: guest=%p, handle=0x%X, returning KTHREAD=0x%llX",
             guest, handle, *result);
    }
}

static void HLE_KeGetCurrentPrcb(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return KPRCB (Processor Control Block) address
    u32 hw_thread = cpu->get_context(0).thread_id % 6;
    *result = 0x80060000 + hw_thread * 0x1000;
}

static void HLE_KeGetCurrentProcessorNumber(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = cpu->get_context(0).thread_id % 6;
}

static void HLE_NtYieldExecution(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    std::this_thread::yield();
    *result = STATUS_SUCCESS;
}

static void HLE_KeTlsAlloc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 slot = g_ext_hle.next_tls_slot++;
    if (slot < 64) {
        *result = slot;
        LOGD("KeTlsAlloc: allocated slot %u", slot);
    } else {
        *result = 0xFFFFFFFF;  // TLS_OUT_OF_INDEXES
        LOGW("KeTlsAlloc: out of slots");
    }
}

static void HLE_KeTlsFree(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 slot = static_cast<u32>(args[0]);
    if (slot < 64) {
        g_ext_hle.tls_slots[slot] = 0;
        *result = 1;  // TRUE
    } else {
        *result = 0;  // FALSE
    }
}

static void HLE_KeTlsGetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 slot = static_cast<u32>(args[0]);
    if (slot < 64) {
        *result = g_ext_hle.tls_slots[slot];
    } else {
        *result = 0;
    }
}

static void HLE_KeTlsSetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 slot = static_cast<u32>(args[0]);
    u64 value = args[1];
    
    if (slot < 64) {
        g_ext_hle.tls_slots[slot] = value;
        *result = 1;  // TRUE
    } else {
        *result = 0;  // FALSE
    }
}

static void HLE_KeSetBasePriorityThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr thread_ptr = static_cast<GuestAddr>(args[0]);
    s32 increment = static_cast<s32>(args[1]);
    
    // Return previous priority increment
    *result = 0;
}

static void HLE_KeSetDisableBoostThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

static void HLE_KeResumeThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr thread_ptr = static_cast<GuestAddr>(args[0]);
    
    // Find thread and decrement suspend count
    // Return previous suspend count
    *result = 1;
}

static void HLE_KeSuspendThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr thread_ptr = static_cast<GuestAddr>(args[0]);
    
    // Return previous suspend count
    *result = 0;
}

static void HLE_NtResumeThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[1]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.thread_mutex);
    
    auto it = g_ext_hle.threads.find(handle);
    if (it != g_ext_hle.threads.end()) {
        u32 prev_count = it->second.suspended ? 1 : 0;
        it->second.suspended = false;
        
        if (prev_count_ptr) {
            memory->write_u32(prev_count_ptr, prev_count);
        }
        *result = STATUS_SUCCESS;
    } else {
        *result = STATUS_INVALID_HANDLE;
    }
}

static void HLE_NtSuspendThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[1]);
    
    std::lock_guard<std::mutex> lock(g_ext_hle.thread_mutex);
    
    auto it = g_ext_hle.threads.find(handle);
    if (it != g_ext_hle.threads.end()) {
        u32 prev_count = it->second.suspended ? 1 : 0;
        it->second.suspended = true;
        
        if (prev_count_ptr) {
            memory->write_u32(prev_count_ptr, prev_count);
        }
        *result = STATUS_SUCCESS;
    } else {
        *result = STATUS_INVALID_HANDLE;
    }
}

//=============================================================================
// Time Functions
//=============================================================================

static void HLE_KeQuerySystemTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr time_ptr = static_cast<GuestAddr>(args[0]);
    
    // Get current time as 100ns intervals since January 1, 1601
    auto now = std::chrono::system_clock::now();
    auto since_epoch = now.time_since_epoch();
    auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count() * 10;
    
    // Add offset from 1601 to 1970 (Unix epoch)
    u64 system_time = static_cast<u64>(ticks) + 116444736000000000ULL;
    
    memory->write_u64(time_ptr, system_time);
    *result = 0;
}

static void HLE_KeQueryInterruptTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return time since boot in 100ns units
    auto now = std::chrono::steady_clock::now();
    auto since_boot = now - g_ext_hle.boot_time;
    u64 ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(since_boot).count() / 100;
    
    *result = ticks;
}

static void HLE_NtQuerySystemTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    HLE_KeQuerySystemTime(cpu, memory, args, result);
    *result = STATUS_SUCCESS;
}

static void HLE_RtlTimeToTimeFields(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr time_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr fields_ptr = static_cast<GuestAddr>(args[1]);
    
    u64 time = memory->read_u64(time_ptr);
    
    // Convert to Unix timestamp
    time_t unix_time = static_cast<time_t>((time - 116444736000000000ULL) / 10000000);
    struct tm* tm_info = gmtime(&unix_time);
    
    if (tm_info) {
        // TIME_FIELDS structure
        memory->write_u16(fields_ptr + 0, static_cast<u16>(tm_info->tm_year + 1900));
        memory->write_u16(fields_ptr + 2, static_cast<u16>(tm_info->tm_mon + 1));
        memory->write_u16(fields_ptr + 4, static_cast<u16>(tm_info->tm_mday));
        memory->write_u16(fields_ptr + 6, static_cast<u16>(tm_info->tm_hour));
        memory->write_u16(fields_ptr + 8, static_cast<u16>(tm_info->tm_min));
        memory->write_u16(fields_ptr + 10, static_cast<u16>(tm_info->tm_sec));
        memory->write_u16(fields_ptr + 12, static_cast<u16>((time / 10000) % 1000));  // Milliseconds
        memory->write_u16(fields_ptr + 14, static_cast<u16>(tm_info->tm_wday));
    }
    
    *result = 0;
}

static void HLE_RtlTimeFieldsToTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr fields_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr time_ptr = static_cast<GuestAddr>(args[1]);
    
    struct tm tm_info = {};
    tm_info.tm_year = memory->read_u16(fields_ptr + 0) - 1900;
    tm_info.tm_mon = memory->read_u16(fields_ptr + 2) - 1;
    tm_info.tm_mday = memory->read_u16(fields_ptr + 4);
    tm_info.tm_hour = memory->read_u16(fields_ptr + 6);
    tm_info.tm_min = memory->read_u16(fields_ptr + 8);
    tm_info.tm_sec = memory->read_u16(fields_ptr + 10);
    
    time_t unix_time = mktime(&tm_info);
    u64 system_time = static_cast<u64>(unix_time) * 10000000 + 116444736000000000ULL;
    
    memory->write_u64(time_ptr, system_time);
    *result = 1;  // TRUE
}

//=============================================================================
// Interlocked Operations
//=============================================================================

static void HLE_InterlockedIncrement(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    s32 value = static_cast<s32>(memory->read_u32(addr));
    value++;
    memory->write_u32(addr, static_cast<u32>(value));
    *result = static_cast<u64>(static_cast<u32>(value));
}

static void HLE_InterlockedDecrement(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    s32 value = static_cast<s32>(memory->read_u32(addr));
    value--;
    memory->write_u32(addr, static_cast<u32>(value));
    *result = static_cast<u64>(static_cast<u32>(value));
}

static void HLE_InterlockedExchange(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 new_val = static_cast<u32>(args[1]);
    
    u32 old_val = memory->read_u32(addr);
    memory->write_u32(addr, new_val);
    *result = old_val;
}

static void HLE_InterlockedCompareExchange(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 exchange = static_cast<u32>(args[1]);
    u32 comparand = static_cast<u32>(args[2]);
    
    u32 current = memory->read_u32(addr);
    if (current == comparand) {
        memory->write_u32(addr, exchange);
    }
    *result = current;
}

static void HLE_InterlockedExchangeAdd(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    s32 value = static_cast<s32>(args[1]);
    
    u32 old_val = memory->read_u32(addr);
    memory->write_u32(addr, old_val + static_cast<u32>(value));
    *result = old_val;
}

static void HLE_InterlockedOr(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 value = static_cast<u32>(args[1]);
    
    u32 old_val = memory->read_u32(addr);
    memory->write_u32(addr, old_val | value);
    *result = old_val;
}

static void HLE_InterlockedAnd(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 value = static_cast<u32>(args[1]);
    
    u32 old_val = memory->read_u32(addr);
    memory->write_u32(addr, old_val & value);
    *result = old_val;
}

//=============================================================================
// Object Management
//=============================================================================

static void HLE_ObReferenceObjectByHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    u32 object_type = static_cast<u32>(args[1]);
    GuestAddr object_ptr = static_cast<GuestAddr>(args[4]);
    
    // Write a pseudo-object pointer based on handle
    memory->write_u32(object_ptr, 0x80000000 + (handle & 0xFFFF) * 0x100);
    
    *result = STATUS_SUCCESS;
}

static void HLE_ObDereferenceObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Decrement reference count - no-op for now
    *result = 0;
}

static void HLE_ObCreateObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 object_type = static_cast<u32>(args[0]);
    GuestAddr object_attr = static_cast<GuestAddr>(args[1]);
    u32 attr_count = static_cast<u32>(args[2]);
    u32 body_size = static_cast<u32>(args[3]);
    GuestAddr object_ptr = static_cast<GuestAddr>(args[4]);
    
    // Allocate object
    static GuestAddr next_object = 0x90000000;
    GuestAddr obj = next_object;
    next_object += align_up(body_size + 0x20, 16u);
    
    memory->write_u32(object_ptr, obj);
    *result = STATUS_SUCCESS;
}

static void HLE_NtDuplicateObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 source_handle = static_cast<u32>(args[1]);
    GuestAddr target_handle_ptr = static_cast<GuestAddr>(args[3]);
    
    // Create a duplicate handle (just use a new handle number)
    u32 new_handle = g_ext_hle.next_handle++;
    memory->write_u32(target_handle_ptr, new_handle);
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Exception Handling
//=============================================================================

static void HLE_RtlUnwind(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

static void HLE_RtlCaptureContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr context_ptr = static_cast<GuestAddr>(args[0]);
    
    // Save current CPU context to the CONTEXT structure
    const auto& ctx = cpu->get_context(0);
    
    // CONTEXT structure layout (simplified)
    // Write GPRs
    for (u32 i = 0; i < 32; i++) {
        memory->write_u64(context_ptr + 0x78 + i * 8, ctx.gpr[i]);
    }
    
    // Write special registers
    memory->write_u64(context_ptr + 0x178, ctx.pc);   // Iar (PC)
    memory->write_u64(context_ptr + 0x180, ctx.lr);   // Lr
    memory->write_u64(context_ptr + 0x188, ctx.ctr);  // Ctr
    
    *result = 0;
}

static void HLE_RtlLookupFunctionEntry(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr pc = static_cast<GuestAddr>(args[0]);
    
    // Return function entry for exception handling
    // 0 = no entry found (leaf function)
    *result = 0;
}

static void HLE_RtlVirtualUnwind(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

//=============================================================================
// String Functions
//=============================================================================

static void HLE_RtlCompareString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr str1 = static_cast<GuestAddr>(args[0]);
    GuestAddr str2 = static_cast<GuestAddr>(args[1]);
    u32 case_sensitive = static_cast<u32>(args[2]);
    
    u16 len1 = memory->read_u16(str1);
    u16 len2 = memory->read_u16(str2);
    GuestAddr buf1 = memory->read_u32(str1 + 4);
    GuestAddr buf2 = memory->read_u32(str2 + 4);
    
    u16 min_len = std::min(len1, len2);
    for (u16 i = 0; i < min_len; i++) {
        char c1 = static_cast<char>(memory->read_u8(buf1 + i));
        char c2 = static_cast<char>(memory->read_u8(buf2 + i));
        
        if (!case_sensitive) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        
        if (c1 != c2) {
            *result = (c1 < c2) ? static_cast<u64>(-1) : 1;
            return;
        }
    }
    
    *result = (len1 == len2) ? 0 : ((len1 < len2) ? static_cast<u64>(-1) : 1);
}

static void HLE_RtlCopyString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dest = static_cast<GuestAddr>(args[0]);
    GuestAddr src = static_cast<GuestAddr>(args[1]);
    
    u16 src_len = memory->read_u16(src);
    u16 dest_max = memory->read_u16(dest + 2);
    GuestAddr src_buf = memory->read_u32(src + 4);
    GuestAddr dest_buf = memory->read_u32(dest + 4);
    
    u16 copy_len = std::min(src_len, dest_max);
    
    for (u16 i = 0; i < copy_len; i++) {
        memory->write_u8(dest_buf + i, memory->read_u8(src_buf + i));
    }
    memory->write_u16(dest, copy_len);
    
    *result = 0;
}

static void HLE_RtlUnicodeStringToAnsiString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr ansi = static_cast<GuestAddr>(args[0]);
    GuestAddr unicode = static_cast<GuestAddr>(args[1]);
    u32 alloc_buffer = static_cast<u32>(args[2]);
    
    u16 uni_len = memory->read_u16(unicode);       // Length in bytes
    GuestAddr uni_buf = memory->read_u32(unicode + 4);
    
    u16 ansi_len = uni_len / 2;
    
    // Simplified conversion - just take low byte of each Unicode char
    GuestAddr ansi_buf;
    if (alloc_buffer) {
        // Allocate buffer
        static GuestAddr next_str_buf = 0x50000000;
        ansi_buf = next_str_buf;
        next_str_buf += align_up(static_cast<u32>(ansi_len + 1), 16u);
    } else {
        ansi_buf = memory->read_u32(ansi + 4);
    }
    
    for (u16 i = 0; i < ansi_len; i++) {
        u16 wc = memory->read_u16(uni_buf + i * 2);
        memory->write_u8(ansi_buf + i, static_cast<u8>(wc & 0xFF));
    }
    memory->write_u8(ansi_buf + ansi_len, 0);
    
    memory->write_u16(ansi, ansi_len);
    memory->write_u16(ansi + 2, ansi_len + 1);
    memory->write_u32(ansi + 4, ansi_buf);
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlAnsiStringToUnicodeString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr unicode = static_cast<GuestAddr>(args[0]);
    GuestAddr ansi = static_cast<GuestAddr>(args[1]);
    u32 alloc_buffer = static_cast<u32>(args[2]);
    
    u16 ansi_len = memory->read_u16(ansi);
    GuestAddr ansi_buf = memory->read_u32(ansi + 4);
    
    u16 uni_len = ansi_len * 2;
    
    GuestAddr uni_buf;
    if (alloc_buffer) {
        static GuestAddr next_str_buf = 0x51000000;
        uni_buf = next_str_buf;
        next_str_buf += align_up(static_cast<u32>(uni_len + 2), 16u);
    } else {
        uni_buf = memory->read_u32(unicode + 4);
    }
    
    for (u16 i = 0; i < ansi_len; i++) {
        u8 c = memory->read_u8(ansi_buf + i);
        memory->write_u16(uni_buf + i * 2, c);
    }
    memory->write_u16(uni_buf + uni_len, 0);
    
    memory->write_u16(unicode, uni_len);
    memory->write_u16(unicode + 2, uni_len + 2);
    memory->write_u32(unicode + 4, uni_buf);
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlFreeAnsiString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Would free the buffer - no-op for our allocator
    *result = 0;
}

static void HLE_RtlFreeUnicodeString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

//=============================================================================
// Random Number Generation
//=============================================================================

static void HLE_RtlRandom(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr seed_ptr = static_cast<GuestAddr>(args[0]);
    u32 seed = memory->read_u32(seed_ptr);
    
    // LCG
    seed = seed * 1103515245 + 12345;
    memory->write_u32(seed_ptr, seed);
    
    *result = (seed >> 16) & 0x7FFF;
}

static void HLE_RtlRandomEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr seed_ptr = static_cast<GuestAddr>(args[0]);
    u32 seed = memory->read_u32(seed_ptr);
    
    // Better RNG
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    memory->write_u32(seed_ptr, seed);
    
    *result = seed & 0x7FFFFFFF;
}

//=============================================================================
// System Functions
//=============================================================================

static void HLE_XeKeysGetKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 key_type = static_cast<u32>(args[0]);
    GuestAddr key_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr size_ptr = static_cast<GuestAddr>(args[2]);
    
    u32 size = memory->read_u32(size_ptr);
    
    // Zero the key
    for (u32 i = 0; i < size && i < 32; i++) {
        memory->write_u8(key_ptr + i, 0);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_XexGetModuleHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr name_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[1]);
    
    // Return main module handle
    memory->write_u32(handle_ptr, 0x80010000);
    *result = STATUS_SUCCESS;
}

static void HLE_XexGetModuleSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 module_handle = static_cast<u32>(args[0]);
    GuestAddr section_name = static_cast<GuestAddr>(args[1]);
    GuestAddr data_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr size_ptr = static_cast<GuestAddr>(args[3]);
    
    // Return success with zero size (section not found)
    if (data_ptr) memory->write_u32(data_ptr, 0);
    if (size_ptr) memory->write_u32(size_ptr, 0);
    
    *result = STATUS_SUCCESS;
}

static void HLE_XexGetProcedureAddress(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 module_handle = static_cast<u32>(args[0]);
    u32 ordinal = static_cast<u32>(args[1]);
    GuestAddr addr_ptr = static_cast<GuestAddr>(args[2]);
    
    // Return 0 (procedure not found)
    memory->write_u32(addr_ptr, 0);
    *result = STATUS_OBJECT_NAME_NOT_FOUND;
}

static void HLE_HalReturnToFirmware(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 reason = static_cast<u32>(args[0]);
    LOGI("HalReturnToFirmware: reason=%u", reason);
    
    // Game wants to exit/reboot
    *result = 0;
}

static void HLE_KeBugCheck(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 code = static_cast<u32>(args[0]);
    LOGE("KeBugCheck: code=0x%08X", code);
    *result = 0;
}

static void HLE_KeBugCheckEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGE("KeBugCheckEx: 0x%08llX, 0x%08llX, 0x%08llX, 0x%08llX, 0x%08llX",
         args[0], args[1], args[2], args[3], args[4]);
    *result = 0;
}

//=============================================================================
// DPC (Deferred Procedure Calls)
//=============================================================================

static void HLE_KeInitializeDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc = static_cast<GuestAddr>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);
    
    // KDPC structure
    memory->write_u8(dpc + 0, 0x13);     // Type = DpcObject
    memory->write_u8(dpc + 1, 0);        // Importance
    memory->write_u16(dpc + 2, 0);       // Number
    memory->write_u32(dpc + 4, 0);       // DpcListEntry.Flink
    memory->write_u32(dpc + 8, 0);       // DpcListEntry.Blink
    memory->write_u32(dpc + 12, routine);
    memory->write_u32(dpc + 16, context);
    memory->write_u32(dpc + 20, 0);      // SystemArgument1
    memory->write_u32(dpc + 24, 0);      // SystemArgument2
    memory->write_u32(dpc + 28, 0);      // DpcData
    
    *result = 0;
}

static void HLE_KeInsertQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc = static_cast<GuestAddr>(args[0]);
    GuestAddr arg1 = static_cast<GuestAddr>(args[1]);
    GuestAddr arg2 = static_cast<GuestAddr>(args[2]);
    
    // Store arguments in the KDPC structure (per Xbox 360 spec)
    // Offset 0x14 = SystemArgument1
    // Offset 0x18 = SystemArgument2
    memory->write_u32(dpc + 0x14, arg1);
    memory->write_u32(dpc + 0x18, arg2);
    
    // Read DPC info from structure
    // Offset 0x0C = DeferredRoutine
    // Offset 0x10 = DeferredContext
    GuestAddr routine = memory->read_u32(dpc + 0x0C);
    GuestAddr context = memory->read_u32(dpc + 0x10);
    
    // Queue the DPC using the central KernelState queue
    // This ensures DPCs are processed in process_dpcs()
    KernelState::instance().queue_dpc(dpc, routine, context, arg1, arg2);
    
    // Also keep local queue for backward compatibility
    {
        std::lock_guard<std::mutex> lock(g_ext_hle.dpc_mutex);
        g_ext_hle.dpc_queue.push_back({routine, context, arg1, arg2});
    }
    
    LOGI("KeInsertQueueDpc: dpc=0x%08X, routine=0x%08X, context=0x%08X, arg1=0x%08X, arg2=0x%08X",
         dpc, routine, context, arg1, arg2);
    
    *result = 1;  // Inserted
}

static void HLE_KeRemoveQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Remove DPC from queue
    *result = 1;  // Removed
}

//=============================================================================
// Timer Functions
//=============================================================================

static void HLE_KeInitializeTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer = static_cast<GuestAddr>(args[0]);
    u32 type = static_cast<u32>(args[1]);
    
    // KTIMER structure
    memory->write_u8(timer + 0, 0x08 + type);  // Type
    memory->write_u8(timer + 1, 0);
    memory->write_u16(timer + 2, sizeof(u32) * 10);
    memory->write_u32(timer + 4, 0);  // SignalState = not signaled
    // Rest is timer-specific
    
    *result = 0;
}

static void HLE_KeSetTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer = static_cast<GuestAddr>(args[0]);
    s64 due_time = static_cast<s64>(args[1]);
    s32 period_ms = static_cast<s32>(args[2]);
    GuestAddr dpc = static_cast<GuestAddr>(args[3]);
    
    // Check if timer was already set
    bool was_set = (memory->read_u32(timer + 4) != 0);  // Check if signaled/active
    
    // Reset signal state
    memory->write_u32(timer + 4, 0);
    
    // Calculate absolute due time in 100ns units
    u64 absolute_due_time;
    u64 current_time = KernelState::instance().system_time();
    
    if (due_time < 0) {
        // Negative = relative time (delay in 100ns units)
        absolute_due_time = current_time + static_cast<u64>(-due_time);
    } else if (due_time == 0) {
        // Zero = fire immediately
        absolute_due_time = current_time;
    } else {
        // Positive = absolute time
        absolute_due_time = static_cast<u64>(due_time);
    }
    
    // Convert period from milliseconds to 100ns units
    u64 period_100ns = static_cast<u64>(period_ms) * 10000ULL;
    
    // Queue the timer
    KernelState::instance().queue_timer(timer, absolute_due_time, period_100ns, dpc);
    
    LOGI("KeSetTimerEx: timer=0x%08X, due_time=%lld (%s), period=%d ms, dpc=0x%08X",
         timer, (long long)due_time, due_time < 0 ? "relative" : "absolute", 
         period_ms, dpc);
    
    // Return whether timer was already set
    *result = was_set ? 1 : 0;
}

static void HLE_KeCancelTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer = static_cast<GuestAddr>(args[0]);
    
    // Cancel the timer and return whether it was set
    bool was_set = KernelState::instance().cancel_timer(timer);
    
    // Also reset the signal state
    memory->write_u32(timer + 4, 0);
    
    LOGI("KeCancelTimer: timer=0x%08X, was_set=%d", timer, was_set ? 1 : 0);
    
    *result = was_set ? 1 : 0;
}

//=============================================================================
// XMA Audio Functions
//=============================================================================

// Global XMA processor pointer (set during kernel init)
static class XmaProcessor* g_xma_processor = nullptr;

void set_xma_processor(class XmaProcessor* processor) {
    g_xma_processor = processor;
}

XmaProcessor* get_xma_processor() {
    return g_xma_processor;
}

static void HLE_XMACreateContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMACreateContext(DWORD* ContextIndex);
    GuestAddr context_index_ptr = static_cast<GuestAddr>(args[0]);
    
    if (!g_xma_processor) {
        LOGE("XMACreateContext: XMA processor not initialized");
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 context_id = g_xma_processor->create_context();
    if (context_id == UINT32_MAX) {
        LOGE("XMACreateContext: failed to create context");
        *result = STATUS_NO_MEMORY;
        return;
    }
    
    memory->write_u32(context_index_ptr, context_id);
    LOGD("XMACreateContext: created context %u", context_id);
    *result = STATUS_SUCCESS;
}

static void HLE_XMADeleteContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMADeleteContext(DWORD ContextIndex);
    u32 context_id = static_cast<u32>(args[0]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    g_xma_processor->destroy_context(context_id);
    LOGD("XMADeleteContext: destroyed context %u", context_id);
    *result = STATUS_SUCCESS;
}

static void HLE_XMASetInputBuffer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMASetInputBuffer(
    //   DWORD ContextIndex,
    //   void* InputBuffer,
    //   DWORD InputBufferSize,
    //   DWORD BufferIndex
    // );
    u32 context_id = static_cast<u32>(args[0]);
    GuestAddr input_buffer = static_cast<GuestAddr>(args[1]);
    u32 input_size = static_cast<u32>(args[2]);
    u32 buffer_index = static_cast<u32>(args[3]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    g_xma_processor->set_input_buffer(context_id, input_buffer, input_size, buffer_index);
    LOGD("XMASetInputBuffer: ctx=%u, buf=%u, addr=0x%08X, size=%u", 
         context_id, buffer_index, input_buffer, input_size);
    *result = STATUS_SUCCESS;
}

static void HLE_XMASetOutputBuffer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMASetOutputBuffer(
    //   DWORD ContextIndex,
    //   void* OutputBuffer,
    //   DWORD OutputBufferSize
    // );
    u32 context_id = static_cast<u32>(args[0]);
    GuestAddr output_buffer = static_cast<GuestAddr>(args[1]);
    u32 output_size = static_cast<u32>(args[2]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    g_xma_processor->set_output_buffer(context_id, output_buffer, output_size);
    LOGD("XMASetOutputBuffer: ctx=%u, addr=0x%08X, size=%u", context_id, output_buffer, output_size);
    *result = STATUS_SUCCESS;
}

static void HLE_XMAEnableContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMAEnableContext(DWORD ContextIndex);
    u32 context_id = static_cast<u32>(args[0]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    g_xma_processor->enable_context(context_id);
    LOGD("XMAEnableContext: ctx=%u", context_id);
    *result = STATUS_SUCCESS;
}

static void HLE_XMADisableContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMADisableContext(DWORD ContextIndex, BOOL Wait);
    u32 context_id = static_cast<u32>(args[0]);
    u32 wait = static_cast<u32>(args[1]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    g_xma_processor->disable_context(context_id);
    LOGD("XMADisableContext: ctx=%u, wait=%u", context_id, wait);
    *result = STATUS_SUCCESS;
}

static void HLE_XMAGetOutputBufferWriteOffset(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMAGetOutputBufferWriteOffset(DWORD ContextIndex, DWORD* WriteOffset);
    u32 context_id = static_cast<u32>(args[0]);
    GuestAddr write_offset_ptr = static_cast<GuestAddr>(args[1]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 offset = g_xma_processor->get_output_write_offset(context_id);
    memory->write_u32(write_offset_ptr, offset);
    *result = STATUS_SUCCESS;
}

static void HLE_XMAIsInputBufferConsumed(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // BOOL XMAIsInputBufferConsumed(DWORD ContextIndex, DWORD BufferIndex);
    u32 context_id = static_cast<u32>(args[0]);
    u32 buffer_index = static_cast<u32>(args[1]);
    
    if (!g_xma_processor) {
        *result = 1;  // TRUE - treat as consumed if no processor
        return;
    }
    
    *result = g_xma_processor->is_input_buffer_consumed(context_id, buffer_index) ? 1 : 0;
}

static void HLE_XMASetContextData(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMASetContextData(DWORD ContextIndex, void* ContextData);
    u32 context_id = static_cast<u32>(args[0]);
    GuestAddr context_data = static_cast<GuestAddr>(args[1]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Read context data structure and configure
    // XMA context data typically includes:
    // +0: Sample rate
    // +4: Channels
    // +8: Loop data
    // etc.
    
    if (context_data) {
        u32 sample_rate = memory->read_u32(context_data + 0);
        u32 channels = memory->read_u32(context_data + 4);
        u32 loop_start = memory->read_u32(context_data + 8);
        u32 loop_end = memory->read_u32(context_data + 12);
        u32 loop_count = memory->read_u32(context_data + 16);
        
        // Apply valid values
        if (sample_rate >= 8000 && sample_rate <= 48000) {
            g_xma_processor->set_context_sample_rate(context_id, sample_rate);
        }
        if (channels >= 1 && channels <= 6) {
            g_xma_processor->set_context_channels(context_id, channels);
        }
        if (loop_count > 0) {
            g_xma_processor->set_context_loop(context_id, true, loop_start, loop_end);
        }
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_XMABlockWhileInUse(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMABlockWhileInUse(DWORD ContextIndex);
    u32 context_id = static_cast<u32>(args[0]);
    
    if (!g_xma_processor) {
        *result = STATUS_SUCCESS;
        return;
    }
    
    // Wait for context to become inactive
    // For now, just process pending data
    while (g_xma_processor->is_context_active(context_id)) {
        g_xma_processor->process_context(context_id, 1);
        std::this_thread::yield();
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_XMAGetContextSampleRate(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD XMAGetContextSampleRate(DWORD ContextIndex, DWORD* SampleRate);
    u32 context_id = static_cast<u32>(args[0]);
    GuestAddr sample_rate_ptr = static_cast<GuestAddr>(args[1]);
    
    if (!g_xma_processor) {
        *result = STATUS_UNSUCCESSFUL;
        return;
    }
    
    auto* ctx = g_xma_processor->get_context(context_id);
    if (ctx) {
        memory->write_u32(sample_rate_ptr, ctx->sample_rate);
        *result = STATUS_SUCCESS;
    } else {
        *result = STATUS_INVALID_PARAMETER;
    }
}

//=============================================================================
// Video Display Functions (Vd*)
// These are critical for GPU initialization!
//=============================================================================

static void HLE_VdGetCurrentDisplayGamma(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Returns current display gamma ramp
    // BOOL VdGetCurrentDisplayGamma(PVOID GammaRamp, DWORD Unknown)
    GuestAddr gamma_ramp = static_cast<GuestAddr>(args[0]);
    
    // Return a default gamma ramp (linear 1.0)
    if (gamma_ramp) {
        // Gamma ramp is 256 entries per channel (R, G, B), each u16
        for (int i = 0; i < 256; i++) {
            u16 value = static_cast<u16>((i << 8) | i);  // Linear ramp
            memory->write_u16(gamma_ramp + i * 2, value);         // R
            memory->write_u16(gamma_ramp + 512 + i * 2, value);   // G
            memory->write_u16(gamma_ramp + 1024 + i * 2, value);  // B
        }
    }
    
    *result = 1;  // Success
    LOGI("VdGetCurrentDisplayGamma: returned linear gamma ramp");
}

static void HLE_VdGetCurrentDisplayInformation(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // BOOL VdGetCurrentDisplayInformation(PVOID DisplayInfo, DWORD InfoSize)
    GuestAddr info = static_cast<GuestAddr>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    
    if (info && size >= 8) {
        // Basic display info: width, height
        memory->write_u32(info, 1280);      // Width
        memory->write_u32(info + 4, 720);   // Height
    }
    
    *result = 1;  // Success
    LOGI("VdGetCurrentDisplayInformation: 1280x720");
}

static void HLE_VdGetCurrentDisplayMode(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdGetCurrentDisplayMode(DWORD* Width, DWORD* Height, DWORD* RefreshRate)
    GuestAddr width_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr height_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr refresh_ptr = static_cast<GuestAddr>(args[2]);
    
    if (width_ptr) memory->write_u32(width_ptr, 1280);
    if (height_ptr) memory->write_u32(height_ptr, 720);
    if (refresh_ptr) memory->write_u32(refresh_ptr, 60);
    
    *result = 0;  // Success
    LOGI("VdGetCurrentDisplayMode: 1280x720 @ 60Hz");
}

static void HLE_VdInitializeEngines(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD VdInitializeEngines(DWORD Unknown1, PVOID Callback, PVOID Unknown2, PVOID Unknown3, PVOID Unknown4)
    // This is critical for GPU initialization!
    LOGI("VdInitializeEngines called - GPU engines initialized");
    *result = 0;  // Success
}

static void HLE_VdInitializeRingBuffer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD VdInitializeRingBuffer(PVOID RingBuffer, DWORD Size)
    GuestAddr ring_buffer = static_cast<GuestAddr>(args[0]);
    u32 size = static_cast<u32>(args[1]);
    
    LOGI("VdInitializeRingBuffer: buffer=0x%08X, size=0x%X", ring_buffer, size);
    *result = 0;  // Success
}

static void HLE_VdSetDisplayMode(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // DWORD VdSetDisplayMode(DWORD Width, DWORD Height, DWORD RefreshRate, DWORD Flags)
    u32 width = static_cast<u32>(args[0]);
    u32 height = static_cast<u32>(args[1]);
    u32 refresh = static_cast<u32>(args[2]);
    
    LOGI("VdSetDisplayMode: %ux%u @ %uHz", width, height, refresh);
    *result = 0;  // Success
}

static void HLE_VdSetDisplayModeOverride(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Similar to VdSetDisplayMode but overrides settings
    *result = 0;  // Success
}

static void HLE_VdQueryVideoFlags(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Returns video capability flags
    // Bit 0: widescreen
    // Bit 1: HDTV capable
    // Bit 2: 480p
    // Bit 3: 720p
    // Bit 4: 1080i
    // Bit 5: 1080p
    *result = 0x3F;  // All features supported
    LOGI("VdQueryVideoFlags: 0x%llX (all features)", *result);
}

static void HLE_VdQueryVideoMode(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdQueryVideoMode(PVOID VideoMode)
    GuestAddr mode_ptr = static_cast<GuestAddr>(args[0]);
    
    if (mode_ptr) {
        // VIDEO_MODE structure:
        // +0: DisplayWidth (u32)
        // +4: DisplayHeight (u32)
        // +8: IsInterlaced (u32)
        // +12: IsWidescreen (u32)
        // +16: IsHighDef (u32)
        // +20: RefreshRate (float)
        // +24: VideoStandard (u32)
        // +28: Unknown (u32)
        // +32: Unknown (u32)
        memory->write_u32(mode_ptr + 0, 1280);    // Width
        memory->write_u32(mode_ptr + 4, 720);     // Height
        memory->write_u32(mode_ptr + 8, 0);       // Progressive
        memory->write_u32(mode_ptr + 12, 1);      // Widescreen
        memory->write_u32(mode_ptr + 16, 1);      // HighDef
        // Write 60.0f as float
        u32 refresh_float;
        float refresh = 60.0f;
        memcpy(&refresh_float, &refresh, sizeof(float));
        memory->write_u32(mode_ptr + 20, refresh_float);
        memory->write_u32(mode_ptr + 24, 1);      // VideoStandard (NTSC=0, PAL=1)
    }
    
    *result = 0;
    LOGI("VdQueryVideoMode: 1280x720p60");
}

static void HLE_VdSetGraphicsInterruptCallback(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdSetGraphicsInterruptCallback(PVOID Callback, PVOID Context)
    GuestAddr callback = static_cast<GuestAddr>(args[0]);
    GuestAddr context = static_cast<GuestAddr>(args[1]);
    
    LOGI("VdSetGraphicsInterruptCallback: callback=0x%08X, context=0x%08X", callback, context);
    // Store these for later use if needed
    *result = 0;
}

static void HLE_VdIsHSIOTrainingSucceeded(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // BOOL VdIsHSIOTrainingSucceeded()
    // Returns whether High-Speed I/O training succeeded
    *result = 1;  // Yes
}

static void HLE_VdPersistDisplay(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdPersistDisplay(PVOID Unknown1, PVOID Unknown2)
    *result = 0;  // Success
}

static void HLE_VdRetrainEDRAMWorker(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdRetrainEDRAMWorker(DWORD Unknown)
    *result = 0;
}

static void HLE_VdRetrainEDRAM(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdRetrainEDRAM(DWORD Unknown1, DWORD Unknown2, DWORD Unknown3, PVOID Unknown4, PVOID Unknown5)
    *result = 0;
}

static void HLE_VdCallGraphicsNotificationRoutines(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdCallGraphicsNotificationRoutines(DWORD Reason)
    *result = 0;
}

static void HLE_VdEnableRingBufferRPtrWriteBack(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdEnableRingBufferRPtrWriteBack(PVOID Address, DWORD BlockSize)
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 block_size = static_cast<u32>(args[1]);
    
    LOGI("VdEnableRingBufferRPtrWriteBack: addr=0x%08X, blockSize=%u", addr, block_size);
    *result = 0;
}

static void HLE_VdSwap(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdSwap(...)
    // This triggers a frame swap/present
    *result = 0;
}

static void HLE_VdEnableDisableClockGating(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // void VdEnableDisableClockGating(BOOL Enable)
    *result = 0;
}

//=============================================================================
// APC (Asynchronous Procedure Calls)
//=============================================================================

static void HLE_KeInitializeApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc = static_cast<GuestAddr>(args[0]);
    GuestAddr thread = static_cast<GuestAddr>(args[1]);
    u32 environment = static_cast<u32>(args[2]);
    GuestAddr kernel_routine = static_cast<GuestAddr>(args[3]);
    GuestAddr rundown_routine = static_cast<GuestAddr>(args[4]);
    GuestAddr normal_routine = static_cast<GuestAddr>(args[5]);
    u32 processor_mode = static_cast<u32>(args[6]);
    GuestAddr normal_context = static_cast<GuestAddr>(args[7]);
    
    // Initialize KAPC structure
    memory->write_u8(apc + 0, 0x12);  // Type = ApcObject
    memory->write_u32(apc + 4, thread);
    memory->write_u32(apc + 16, kernel_routine);
    memory->write_u32(apc + 20, rundown_routine);
    memory->write_u32(apc + 24, normal_routine);
    memory->write_u32(apc + 28, normal_context);
    
    *result = 0;
}

static void HLE_KeInsertQueueApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc = static_cast<GuestAddr>(args[0]);
    GuestAddr arg1 = static_cast<GuestAddr>(args[1]);
    GuestAddr arg2 = static_cast<GuestAddr>(args[2]);
    u32 increment = static_cast<u32>(args[3]);
    
    // Store arguments
    memory->write_u32(apc + 32, arg1);
    memory->write_u32(apc + 36, arg2);
    
    *result = 1;  // Inserted
}

//=============================================================================
// Registration
//=============================================================================

void Kernel::register_xboxkrnl_extended() {
    // Initialize extended HLE state
    g_ext_hle.init();
    
    // Memory
    hle_functions_[make_import_key(0, 113)] = HLE_MmAllocatePhysicalMemory;
    hle_functions_[make_import_key(0, 114)] = HLE_MmAllocatePhysicalMemoryEx;
    hle_functions_[make_import_key(0, 116)] = HLE_MmFreePhysicalMemory;
    hle_functions_[make_import_key(0, 117)] = HLE_MmGetPhysicalAddress;
    hle_functions_[make_import_key(0, 118)] = HLE_MmMapIoSpace;
    hle_functions_[make_import_key(0, 119)] = HLE_MmUnmapIoSpace;
    hle_functions_[make_import_key(0, 120)] = HLE_MmQueryAddressProtect;
    hle_functions_[make_import_key(0, 121)] = HLE_MmSetAddressProtect;
    hle_functions_[make_import_key(0, 122)] = HLE_MmQueryAllocationSize;
    hle_functions_[make_import_key(0, 123)] = HLE_MmQueryStatistics;
    
    // Thread
    hle_functions_[make_import_key(0, 14)] = HLE_ExCreateThread;
    hle_functions_[make_import_key(0, 17)] = HLE_ExTerminateThread;
    hle_functions_[make_import_key(0, 216)] = HLE_NtTerminateThread;
    hle_functions_[make_import_key(0, 51)] = HLE_KeGetCurrentThread;
    hle_functions_[make_import_key(0, 50)] = HLE_KeGetCurrentPrcb;
    hle_functions_[make_import_key(0, 49)] = HLE_KeGetCurrentProcessorNumber;
    hle_functions_[make_import_key(0, 221)] = HLE_NtYieldExecution;
    hle_functions_[make_import_key(0, 330)] = HLE_KeTlsAlloc;
    hle_functions_[make_import_key(0, 331)] = HLE_KeTlsFree;
    hle_functions_[make_import_key(0, 332)] = HLE_KeTlsGetValue;
    hle_functions_[make_import_key(0, 333)] = HLE_KeTlsSetValue;
    hle_functions_[make_import_key(0, 78)] = HLE_KeSetBasePriorityThread;
    hle_functions_[make_import_key(0, 79)] = HLE_KeSetDisableBoostThread;
    hle_functions_[make_import_key(0, 75)] = HLE_KeResumeThread;
    hle_functions_[make_import_key(0, 85)] = HLE_KeSuspendThread;
    hle_functions_[make_import_key(0, 209)] = HLE_NtResumeThread;
    hle_functions_[make_import_key(0, 215)] = HLE_NtSuspendThread;
    
    // Time
    hle_functions_[make_import_key(0, 104)] = HLE_KeQuerySystemTime;
    hle_functions_[make_import_key(0, 101)] = HLE_KeQueryInterruptTime;
    hle_functions_[make_import_key(0, 208)] = HLE_NtQuerySystemTime;
    hle_functions_[make_import_key(0, 288)] = HLE_RtlTimeToTimeFields;
    hle_functions_[make_import_key(0, 287)] = HLE_RtlTimeFieldsToTime;
    
    // Interlocked
    hle_functions_[make_import_key(0, 46)] = HLE_InterlockedIncrement;
    hle_functions_[make_import_key(0, 45)] = HLE_InterlockedDecrement;
    hle_functions_[make_import_key(0, 44)] = HLE_InterlockedExchange;
    hle_functions_[make_import_key(0, 43)] = HLE_InterlockedCompareExchange;
    hle_functions_[make_import_key(0, 42)] = HLE_InterlockedExchangeAdd;
    hle_functions_[make_import_key(0, 47)] = HLE_InterlockedOr;
    hle_functions_[make_import_key(0, 41)] = HLE_InterlockedAnd;
    
    // Object
    hle_functions_[make_import_key(0, 140)] = HLE_ObReferenceObjectByHandle;
    hle_functions_[make_import_key(0, 139)] = HLE_ObDereferenceObject;
    hle_functions_[make_import_key(0, 138)] = HLE_ObCreateObject;
    hle_functions_[make_import_key(0, 192)] = HLE_NtDuplicateObject;
    
    // Exception
    hle_functions_[make_import_key(0, 291)] = HLE_RtlUnwind;
    hle_functions_[make_import_key(0, 267)] = HLE_RtlCaptureContext;
    hle_functions_[make_import_key(0, 281)] = HLE_RtlLookupFunctionEntry;
    hle_functions_[make_import_key(0, 292)] = HLE_RtlVirtualUnwind;
    
    // String
    hle_functions_[make_import_key(0, 268)] = HLE_RtlCompareString;
    hle_functions_[make_import_key(0, 270)] = HLE_RtlCopyString;
    hle_functions_[make_import_key(0, 291)] = HLE_RtlUnicodeStringToAnsiString;
    hle_functions_[make_import_key(0, 264)] = HLE_RtlAnsiStringToUnicodeString;
    hle_functions_[make_import_key(0, 273)] = HLE_RtlFreeAnsiString;
    hle_functions_[make_import_key(0, 274)] = HLE_RtlFreeUnicodeString;
    
    // Random
    hle_functions_[make_import_key(0, 283)] = HLE_RtlRandom;
    hle_functions_[make_import_key(0, 284)] = HLE_RtlRandomEx;
    
    // System
    hle_functions_[make_import_key(0, 420)] = HLE_XeKeysGetKey;
    hle_functions_[make_import_key(0, 405)] = HLE_XexGetModuleHandle;
    hle_functions_[make_import_key(0, 406)] = HLE_XexGetModuleSection;
    hle_functions_[make_import_key(0, 407)] = HLE_XexGetProcedureAddress;
    hle_functions_[make_import_key(0, 27)] = HLE_HalReturnToFirmware;
    hle_functions_[make_import_key(0, 336)] = HLE_KeBugCheck;
    hle_functions_[make_import_key(0, 337)] = HLE_KeBugCheckEx;
    
    // DPC
    hle_functions_[make_import_key(0, 57)] = HLE_KeInitializeDpc;
    hle_functions_[make_import_key(0, 62)] = HLE_KeInsertQueueDpc;
    hle_functions_[make_import_key(0, 74)] = HLE_KeRemoveQueueDpc;
    
    // Timer
    hle_functions_[make_import_key(0, 63)] = HLE_KeInitializeTimerEx;
    hle_functions_[make_import_key(0, 86)] = HLE_KeSetTimerEx;
    hle_functions_[make_import_key(0, 38)] = HLE_KeCancelTimer;
    
    // APC
    hle_functions_[make_import_key(0, 54)] = HLE_KeInitializeApc;
    hle_functions_[make_import_key(0, 61)] = HLE_KeInsertQueueApc;
    
    // Video Display Functions (Vd*) - CRITICAL for GPU initialization!
    // These ordinals are from the official Xbox 360 kernel
    hle_functions_[make_import_key(0, 441)] = HLE_VdGetCurrentDisplayGamma;
    hle_functions_[make_import_key(0, 442)] = HLE_VdGetCurrentDisplayInformation;
    hle_functions_[make_import_key(0, 444)] = HLE_VdGetCurrentDisplayMode;
    hle_functions_[make_import_key(0, 445)] = HLE_VdEnableDisableClockGating;
    hle_functions_[make_import_key(0, 450)] = HLE_VdInitializeEngines;  // Most critical!
    hle_functions_[make_import_key(0, 451)] = HLE_VdInitializeRingBuffer;
    hle_functions_[make_import_key(0, 452)] = HLE_VdEnableRingBufferRPtrWriteBack;
    hle_functions_[make_import_key(0, 455)] = HLE_VdIsHSIOTrainingSucceeded;
    hle_functions_[make_import_key(0, 462)] = HLE_VdPersistDisplay;
    hle_functions_[make_import_key(0, 463)] = HLE_VdRetrainEDRAMWorker;
    hle_functions_[make_import_key(0, 464)] = HLE_VdRetrainEDRAM;
    hle_functions_[make_import_key(0, 465)] = HLE_VdCallGraphicsNotificationRoutines;
    hle_functions_[make_import_key(0, 467)] = HLE_VdSetDisplayMode;
    hle_functions_[make_import_key(0, 468)] = HLE_VdSetDisplayModeOverride;
    hle_functions_[make_import_key(0, 469)] = HLE_VdQueryVideoFlags;
    hle_functions_[make_import_key(0, 470)] = HLE_VdQueryVideoMode;
    hle_functions_[make_import_key(0, 471)] = HLE_VdSetGraphicsInterruptCallback;
    hle_functions_[make_import_key(0, 474)] = HLE_VdSwap;
    
    // XMA Audio Functions - CORRECT ordinals (548+)
    hle_functions_[make_import_key(0, 548)] = HLE_XMACreateContext;
    hle_functions_[make_import_key(0, 549)] = HLE_XMADeleteContext;
    hle_functions_[make_import_key(0, 550)] = HLE_XMASetInputBuffer;
    hle_functions_[make_import_key(0, 551)] = HLE_XMASetOutputBuffer;
    hle_functions_[make_import_key(0, 552)] = HLE_XMAEnableContext;
    hle_functions_[make_import_key(0, 553)] = HLE_XMADisableContext;
    hle_functions_[make_import_key(0, 554)] = HLE_XMAGetOutputBufferWriteOffset;
    hle_functions_[make_import_key(0, 555)] = HLE_XMAIsInputBufferConsumed;
    hle_functions_[make_import_key(0, 556)] = HLE_XMASetContextData;
    hle_functions_[make_import_key(0, 557)] = HLE_XMABlockWhileInUse;
    hle_functions_[make_import_key(0, 558)] = HLE_XMAGetContextSampleRate;
    
    // High ordinal functions (from newer SDK versions)
    // 2168: KeSetEventBoostPriority - sets event to signaled state and boosts priority
    // LONG KeSetEventBoostPriority(PRKEVENT Event, KPRIORITY Increment)
    // Returns previous signal state
    static u32 boost_call_count = 0;
    hle_functions_[make_import_key(0, 2168)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        GuestAddr event = static_cast<GuestAddr>(args[0]);
        boost_call_count++;
        
        // Handle special pseudo-handles
        // Handle 1 = NtCurrentThread - resolve to current thread's event
        // Handle 2 = NtCurrentProcess
        if (event == 1 || event == 2) {
            // When game passes handle 1 (current thread), it expects the thread's
            // built-in event to be signaled. Just return success - the thread is "ready"
            *result = 1;  // Previous state was signaled
            return;
        }
        
        if (event == 0) {
            *result = 1;
            return;
        }
        
        // Validate event pointer - must be in valid memory range
        // Valid ranges include:
        // - 0x70000000+ : Stack/heap allocations
        // - 0x80000000+ : Executable/data sections
        // - 0x90000000+ : Additional data
        if (event < 0x10000 || event >= 0xE0000000) {
            // Invalid event pointer (too low or in reserved area), treat as no-op
            *result = 1;
            return;
        }
        
        // Read previous signal state from dispatcher header (+4)
        s32 prev_state = static_cast<s32>(memory->read_u32(event + 4));
        
        // Set event to signaled state (signal_state = 1)
        memory->write_u32(event + 4, 1);
        
        // WORKER THREAD SIMULATION: Set completion flag for Call of Duty style work requests
        // 
        // Call of Duty uses a custom work scheduling pattern:
        // 1. Game creates a work request structure on stack (at r31)
        // 2. Event object is at r31 + 0x50, completion flag is at r31 + 0x14C
        // 3. Game calls KeSetEventBoostPriority(event) to "wake workers"
        // 4. Worker threads are supposed to process work and set completion flag
        // 5. Game polls completion flag (at event + 0xFC) until non-zero
        //
        // Since we don't have real kernel code for worker threads, we simulate
        // the work completion by setting the flag when the event is signaled.
        //
        // The offset 0xFC = 0x14C - 0x50 (completion_flag_offset - event_offset)
        // The game's work completion check is more complex than just "flag == 0"
        // Looking at the disassembly, the game stores r30 to the completion flag
        // BEFORE entering the loop, then checks if it's still that value.
        // 
        // The game expects the worker to CHANGE the value, not just check for zero.
        // Since we see current_val = 65536 (0x10000), that's the initial value.
        // We need to change it to something else (like setting a "done" bit).
        {
            GuestAddr completion_flag = event + 0xFC;
            
            // Set completion flag to non-zero to indicate work is done
            // Valid address ranges: stack (0x70000000+) and data (0x80000000+)
            if (completion_flag >= 0x70000000 && completion_flag < 0xE0000000) {
                u32 current_val = memory->read_u32(completion_flag);
                // Set bit 0 to indicate "done" while preserving original value
                u32 new_val = current_val | 0x1;
                
                // CRITICAL DEBUG: Also read directly from fastmem to check for mismatch
                static int flag_log = 0;
                if (flag_log++ < 100) {
                    // Get raw fastmem value for comparison
                    u8* fastmem = static_cast<u8*>(memory->get_fastmem_base());
                    GuestAddr phys = completion_flag & 0x1FFFFFFF;
                    u32 raw_val = 0;
                    if (fastmem && phys < 0x20000000) {
                        memcpy(&raw_val, fastmem + phys, 4);
                        // raw_val is in big-endian, need to byteswap
                        raw_val = __builtin_bswap32(raw_val);
                    }
                    LOGI("WorkerSim: flag=0x%08X phys=0x%08X read=0x%08X raw=0x%08X match=%d (call #%d)", 
                         (u32)completion_flag, (u32)phys, current_val, raw_val, 
                         (current_val == raw_val), flag_log);
                }
                
                // ALWAYS write a very obvious non-zero value
                u32 done_val = 0xDEADBEEF;  // Very obvious value
                memory->write_u32(completion_flag, done_val);
                
                // Verify the write worked
                u32 verify = memory->read_u32(completion_flag);
                u8* fastmem = static_cast<u8*>(memory->get_fastmem_base());
                GuestAddr phys = completion_flag & 0x1FFFFFFF;
                u32 raw_verify = 0;
                if (fastmem && phys < 0x20000000) {
                    memcpy(&raw_verify, fastmem + phys, 4);
                    raw_verify = __builtin_bswap32(raw_verify);
                }
                
                static int write_log = 0;
                if (write_log++ < 20) {
                    LOGI("WorkerSim: WROTE 0x%08X, verify_read=0x%08X, raw=0x%08X, match=%d", 
                         done_val, verify, raw_verify, (verify == raw_verify && verify == done_val));
                }
            }
        }
        
        // CRITICAL: Process pending DPCs immediately when an event is signaled
        // This simulates the system thread responding to the event and executing
        // any pending work items. Without this, games get stuck waiting for
        // DPC completion that never happens.
        KernelState::instance().process_dpcs();
        
        // Wake any threads waiting on this event
        if (g_scheduler) {
            g_scheduler->signal_object(event);
        }
        
        *result = prev_state;
    };
    
    // 2144 (0x860): ObReferenceObjectByHandle - get object pointer from handle
    // Xbox 360 uses variable calling conventions - safely read only first 4 args
    // The output pointer location varies by SDK version
    hle_functions_[make_import_key(0, 2144)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        GuestAddr handle = static_cast<GuestAddr>(args[0]);
        GuestAddr arg1 = static_cast<GuestAddr>(args[1]);
        GuestAddr arg2 = static_cast<GuestAddr>(args[2]);
        GuestAddr arg3 = static_cast<GuestAddr>(args[3]);
        
        // Determine output pointer - could be arg2, arg3, or arg1 depending on signature
        // Check which looks like a valid writable address
        GuestAddr object_ptr_addr = 0;
        if (arg3 >= 0x70000000 && arg3 < 0xA0000000) {
            object_ptr_addr = arg3;  // Likely the output pointer
        } else if (arg2 >= 0x70000000 && arg2 < 0xA0000000) {
            object_ptr_addr = arg2;
        } else if (arg1 >= 0x70000000 && arg1 < 0xA0000000) {
            object_ptr_addr = arg1;
        }
        
        // Create a fake object address based on handle
        GuestAddr fake_object = 0xA0000000 + (handle * 0x100);
        
        // Write fake object to output pointer if we found one
        if (object_ptr_addr != 0 && object_ptr_addr >= 0x70000000) {
            memory->write_u32(object_ptr_addr, fake_object);
        }
        
        // Initialize the fake object's DISPATCHER_HEADER to look like a signaled event
        memory->write_u8(fake_object, 0);      // Type = NotificationEvent
        memory->write_u8(fake_object + 1, 0);  // Absolute
        memory->write_u8(fake_object + 2, 4);  // Size
        memory->write_u8(fake_object + 3, 0);  // Inserted
        memory->write_u32(fake_object + 4, 1); // SignalState = 1 (SIGNALED!)
        memory->write_u32(fake_object + 8, fake_object + 8);  // WaitListHead.Flink
        memory->write_u32(fake_object + 12, fake_object + 8); // WaitListHead.Blink
        
        *result = 0;  // STATUS_SUCCESS
    };
    
    // 2508: KfAcquireSpinLock - fast spinlock acquire
    hle_functions_[make_import_key(0, 2508)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        // Stub - spinlocks are not needed in single-threaded emulation
        *result = 0;  // Return old IRQL
    };
    
    // Ordinal 0: Null/no-op syscall - games sometimes call this
    hle_functions_[make_import_key(0, 0)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;  // Success
    };
    
    // 1708 (0x6AC): Unknown kernel function
    hle_functions_[make_import_key(0, 1708)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 1712-1720 range: Various kernel functions
    for (u32 ord = 1712; ord <= 1720; ord += 4) {
        hle_functions_[make_import_key(0, ord)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
            *result = 0;
        };
    }
    
    // 2248 (0x8C8): Unknown - likely KiApcNormalRoutineNop or similar
    hle_functions_[make_import_key(0, 2248)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 2252 (0x8CC): Unknown kernel function
    hle_functions_[make_import_key(0, 2252)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 2256 (0x8D0): Unknown kernel function
    hle_functions_[make_import_key(0, 2256)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 2260 (0x8D4): RtlIsPartialPlaceholderFileInfo
    // Returns FALSE - file is not a partial placeholder
    hle_functions_[make_import_key(0, 2260)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;  // FALSE - not a placeholder file
    };
    
    // 2504 (0x9C8): Unknown kernel function
    hle_functions_[make_import_key(0, 2504)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 2512 (0x9D0): Unknown kernel function
    hle_functions_[make_import_key(0, 2512)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;
    };
    
    // 2516 (0x9D4): Unknown kernel function - stub
    hle_functions_[make_import_key(0, 2516)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        *result = 0;  // STATUS_SUCCESS
    };
    
    // 2520 (0x9D8): SeImpersonateClientEx - security impersonation
    hle_functions_[make_import_key(0, 2520)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        // Stub - security impersonation not needed in emulation
        *result = 0;  // STATUS_SUCCESS
    };
    
    // 2528: KfReleaseSpinLock - fast spinlock release
    hle_functions_[make_import_key(0, 2528)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        // Stub - nothing to do
    };
    
    // 2532: XexGetModuleHandle - get handle to loaded module
    hle_functions_[make_import_key(0, 2532)] = [](Cpu* cpu, Memory* memory, u64* args, u64* result) {
        // args[0] = module name pointer (ANSI string)
        // args[1] = output module handle pointer
        GuestAddr name_ptr = static_cast<GuestAddr>(args[0]);
        GuestAddr handle_out = static_cast<GuestAddr>(args[1]);
        
        // Read module name
        char name[256] = {0};
        for (int i = 0; i < 255; i++) {
            name[i] = memory->read_u8(name_ptr + i);
            if (name[i] == 0) break;
        }
        
        // For now, return a fake handle for any module
        // The actual handle value doesn't matter much as long as it's non-zero
        u32 fake_handle = 0x80000000 | (name[0] << 8) | name[1];
        
        if (handle_out) {
            memory->write_u32(handle_out, fake_handle);
        }
        
        *result = 0;  // STATUS_SUCCESS
    };
    
    LOGI("Registered extended xboxkrnl.exe HLE functions (including XMA audio)");
}

// Set scheduler pointer for thread management
void set_thread_scheduler(ThreadScheduler* scheduler) {
    g_scheduler = scheduler;
}

// Get scheduler pointer for thread management (used by APC functions)
ThreadScheduler* get_thread_scheduler() {
    return g_scheduler;
}

} // namespace x360mu
