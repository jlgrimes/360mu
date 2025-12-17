/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Extended xboxkrnl.exe HLE functions
 * Additional kernel functions needed for game compatibility
 */

#include "../kernel.h"
#include "../../cpu/xenon/cpu.h"
#include "../../memory/memory.h"
#include <cstring>
#include <ctime>
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-hle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[HLE] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[HLE WARN] " __VA_ARGS__); printf("\n")
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

// Global state for HLE
static struct {
    u64 system_time;           // 100ns units since 1601
    u64 performance_counter;
    u32 next_handle;
    u32 next_thread_id;
} g_kernel_state = {
    .system_time = 0x01D4A2E0'00000000ULL,  // Approximate time
    .performance_counter = 0,
    .next_handle = 0x80000100,
    .next_thread_id = 1,
};

//=============================================================================
// Extended Memory Functions
//=============================================================================

static void HLE_MmAllocatePhysicalMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // PVOID MmAllocatePhysicalMemory(ULONG Flags, SIZE_T Size, ULONG Protect);
    u32 size = static_cast<u32>(args[1]);
    
    // Align to page size
    size = align_up(size, static_cast<u32>(memory::PAGE_SIZE));
    
    static GuestAddr next_phys = 0xC0000000;  // Physical memory region
    GuestAddr addr = next_phys;
    next_phys += size;
    
    memory->allocate(addr, size, MemoryRegion::Read | MemoryRegion::Write);
    
    *result = addr;
    LOGD("MmAllocatePhysicalMemory: size=0x%X -> 0x%08llX", size, addr);
}

static void HLE_MmFreePhysicalMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[1]);
    memory->free(addr);
    *result = STATUS_SUCCESS;
}

static void HLE_MmGetPhysicalAddress(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Simple identity mapping
    *result = args[0];
}

static void HLE_MmQueryAddressProtect(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return PAGE_READWRITE for simplicity
    *result = 0x04;  // PAGE_READWRITE
}

static void HLE_MmSetAddressProtect(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Set memory protection - simplified
    *result = 0;
}

static void HLE_MmQueryAllocationSize(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    MemoryRegion region;
    if (memory->query(static_cast<GuestAddr>(args[0]), region)) {
        *result = region.size;
    } else {
        *result = 0;
    }
}

//=============================================================================
// Thread and Process Functions
//=============================================================================

static void HLE_ExCreateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Create a new thread
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 stack_size = static_cast<u32>(args[1]);
    GuestAddr thread_id_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr start_addr = static_cast<GuestAddr>(args[3]);
    GuestAddr param = static_cast<GuestAddr>(args[4]);
    u32 flags = static_cast<u32>(args[5]);
    
    // Generate handle and thread ID
    u32 handle = g_kernel_state.next_handle++;
    u32 thread_id = g_kernel_state.next_thread_id++;
    
    // Write handle
    memory->write_u32(handle_ptr, handle);
    
    // Write thread ID if requested
    if (thread_id_ptr) {
        memory->write_u32(thread_id_ptr, thread_id);
    }
    
    // TODO: Actually create and start the thread
    LOGI("ExCreateThread: handle=0x%X, entry=0x%08X, param=0x%08X", 
         handle, start_addr, param);
    
    *result = STATUS_SUCCESS;
}

static void HLE_ExTerminateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 exit_code = static_cast<u32>(args[0]);
    LOGI("ExTerminateThread: exit_code=%u", exit_code);
    *result = STATUS_SUCCESS;
}

static void HLE_KeGetCurrentThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return a fake thread handle
    *result = 0x80000001;
}

static void HLE_KeGetCurrentPrcb(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return processor control block address
    // This is used internally by the kernel
    static GuestAddr fake_prcb = 0x80070000;
    *result = fake_prcb;
}

static void HLE_KeGetCurrentProcessorNumber(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return processor ID (0-5 for Xbox 360)
    *result = 0;
}

static void HLE_KeTlsAlloc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Allocate TLS slot
    static u32 next_tls_slot = 0;
    *result = next_tls_slot++;
}

static void HLE_KeTlsFree(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = STATUS_SUCCESS;
}

static void HLE_KeTlsGetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // TLS value - would need per-thread storage
    *result = 0;
}

static void HLE_KeTlsSetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = STATUS_SUCCESS;
}

static void HLE_KeSetBasePriorityThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;  // Previous priority
}

static void HLE_KeSetDisableBoostThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

static void HLE_KeResumeThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 1;  // Previous suspend count
}

static void HLE_KeSuspendThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;  // Previous suspend count
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
    u64 system_time = ticks + 116444736000000000ULL;
    
    memory->write_u64(time_ptr, system_time);
    *result = 0;
}

static void HLE_KeQueryInterruptTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return interrupt time (100ns ticks since boot)
    g_kernel_state.performance_counter += 10000;
    *result = g_kernel_state.performance_counter;
}

static void HLE_NtQuerySystemTime(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    HLE_KeQuerySystemTime(cpu, memory, args, result);
    *result = STATUS_SUCCESS;
}

static void HLE_RtlTimeToTimeFields(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr time_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr fields_ptr = static_cast<GuestAddr>(args[1]);
    
    u64 time = memory->read_u64(time_ptr);
    
    // Convert to time fields (simplified)
    // TIME_FIELDS: Year(2), Month(2), Day(2), Hour(2), Minute(2), Second(2), Ms(2), Weekday(2)
    time_t unix_time = (time - 116444736000000000ULL) / 10000000;
    struct tm* tm_info = gmtime(&unix_time);
    
    if (tm_info) {
        memory->write_u16(fields_ptr + 0, tm_info->tm_year + 1900);
        memory->write_u16(fields_ptr + 2, tm_info->tm_mon + 1);
        memory->write_u16(fields_ptr + 4, tm_info->tm_mday);
        memory->write_u16(fields_ptr + 6, tm_info->tm_hour);
        memory->write_u16(fields_ptr + 8, tm_info->tm_min);
        memory->write_u16(fields_ptr + 10, tm_info->tm_sec);
        memory->write_u16(fields_ptr + 12, 0);  // Milliseconds
        memory->write_u16(fields_ptr + 14, tm_info->tm_wday);
    }
    
    *result = 0;
}

//=============================================================================
// Interlocked Operations
//=============================================================================

static void HLE_InterlockedIncrement(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 value = memory->read_u32(addr);
    value++;
    memory->write_u32(addr, value);
    *result = value;
}

static void HLE_InterlockedDecrement(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr addr = static_cast<GuestAddr>(args[0]);
    u32 value = memory->read_u32(addr);
    value--;
    memory->write_u32(addr, value);
    *result = value;
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
    memory->write_u32(addr, old_val + value);
    *result = old_val;
}

//=============================================================================
// Object Management
//=============================================================================

static void HLE_ObReferenceObjectByHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr object_ptr = static_cast<GuestAddr>(args[4]);
    
    // Write a fake object pointer
    memory->write_u32(object_ptr, 0x80000000 + (handle & 0xFFFF));
    
    *result = STATUS_SUCCESS;
}

static void HLE_ObDereferenceObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

static void HLE_ObCreateObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr object_ptr = static_cast<GuestAddr>(args[6]);
    
    // Allocate object
    static GuestAddr next_object = 0x90000000;
    GuestAddr obj = next_object;
    next_object += 0x100;
    
    memory->write_u32(object_ptr, obj);
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Exception Handling
//=============================================================================

static void HLE_RtlUnwind(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Stack unwinding for exception handling
    *result = 0;
}

static void HLE_RtlCaptureContext(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Capture current CPU context - would need to store registers
    *result = 0;
}

static void HLE_RtlLookupFunctionEntry(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return function entry for exception handling
    *result = 0;  // No entry found
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
    
    // Read string structures
    u16 len1 = memory->read_u16(str1);
    u16 len2 = memory->read_u16(str2);
    GuestAddr buf1 = memory->read_u32(str1 + 4);
    GuestAddr buf2 = memory->read_u32(str2 + 4);
    
    // Compare character by character
    u16 min_len = std::min(len1, len2);
    for (u16 i = 0; i < min_len; i++) {
        char c1 = static_cast<char>(memory->read_u8(buf1 + i));
        char c2 = static_cast<char>(memory->read_u8(buf2 + i));
        
        if (!case_sensitive) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        
        if (c1 != c2) {
            *result = (c1 < c2) ? -1 : 1;
            return;
        }
    }
    
    *result = (len1 == len2) ? 0 : ((len1 < len2) ? -1 : 1);
}

static void HLE_RtlCompareStringN(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    HLE_RtlCompareString(cpu, memory, args, result);
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
    // Convert Unicode to ANSI - simplified
    *result = STATUS_SUCCESS;
}

static void HLE_RtlAnsiStringToUnicodeString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Convert ANSI to Unicode - simplified
    *result = STATUS_SUCCESS;
}

static void HLE_RtlFreeAnsiString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
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
    
    // Simple LCG
    seed = seed * 1103515245 + 12345;
    memory->write_u32(seed_ptr, seed);
    
    *result = (seed >> 16) & 0x7FFF;
}

static void HLE_RtlRandomEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    HLE_RtlRandom(cpu, memory, args, result);
}

//=============================================================================
// Misc System Functions
//=============================================================================

static void HLE_XeKeysGetKey(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return encryption keys - return fake/zero keys
    GuestAddr key_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr size_ptr = static_cast<GuestAddr>(args[2]);
    
    // Zero the key
    u32 size = memory->read_u32(size_ptr);
    for (u32 i = 0; i < size && i < 32; i++) {
        memory->write_u8(key_ptr + i, 0);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_XexGetModuleHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Get handle to loaded XEX module
    *result = 0x80010000;  // Fake module handle
}

static void HLE_XexGetModuleSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Get section info from XEX
    *result = STATUS_SUCCESS;
}

static void HLE_XexGetProcedureAddress(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Get address of exported procedure
    *result = 0;  // Not found
}

static void HLE_HalReturnToFirmware(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Request to return to dashboard/firmware
    LOGI("HalReturnToFirmware called - game wants to exit");
    *result = 0;
}

static void HLE_KeBugCheck(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Blue screen of death
    LOGI("KeBugCheck called with code 0x%08llX", args[0]);
    *result = 0;
}

static void HLE_KeBugCheckEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    LOGI("KeBugCheckEx: 0x%08llX, 0x%08llX, 0x%08llX, 0x%08llX, 0x%08llX",
         args[0], args[1], args[2], args[3], args[4]);
    *result = 0;
}

//=============================================================================
// DPC (Deferred Procedure Calls)
//=============================================================================

static void HLE_KeInitializeDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Initialize DPC object
    GuestAddr dpc = static_cast<GuestAddr>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);
    
    memory->write_u32(dpc + 0, 0x13);     // Type = DpcObject
    memory->write_u32(dpc + 4, routine);
    memory->write_u32(dpc + 8, context);
    
    *result = 0;
}

static void HLE_KeInsertQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Queue a DPC
    *result = 1;  // Inserted
}

static void HLE_KeRemoveQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 1;  // Removed
}

//=============================================================================
// Timer Functions
//=============================================================================

static void HLE_KeInitializeTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer = static_cast<GuestAddr>(args[0]);
    u32 type = static_cast<u32>(args[1]);
    
    memory->write_u32(timer + 0, 0x08 + type);  // Type = TimerNotificationObject or SynchronizationObject
    memory->write_u32(timer + 4, 0);            // SignalState = 0
    
    *result = 0;
}

static void HLE_KeSetTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Set timer - simplified
    *result = 0;  // Timer was not set before
}

static void HLE_KeCancelTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;  // Timer was not set
}

//=============================================================================
// APC (Asynchronous Procedure Calls)
//=============================================================================

static void HLE_KeInitializeApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 0;
}

static void HLE_KeInsertQueueApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = 1;
}

//=============================================================================
// Registry all extended xboxkrnl.exe functions
//=============================================================================

void Kernel::register_xboxkrnl_extended() {
    // Memory
    hle_functions_[make_import_key(0, 113)] = HLE_MmAllocatePhysicalMemory;
    hle_functions_[make_import_key(0, 116)] = HLE_MmFreePhysicalMemory;
    hle_functions_[make_import_key(0, 117)] = HLE_MmGetPhysicalAddress;
    hle_functions_[make_import_key(0, 119)] = HLE_MmQueryAddressProtect;
    hle_functions_[make_import_key(0, 121)] = HLE_MmSetAddressProtect;
    hle_functions_[make_import_key(0, 120)] = HLE_MmQueryAllocationSize;
    
    // Thread
    hle_functions_[make_import_key(0, 15)] = HLE_ExCreateThread;
    hle_functions_[make_import_key(0, 17)] = HLE_ExTerminateThread;
    hle_functions_[make_import_key(0, 51)] = HLE_KeGetCurrentThread;
    hle_functions_[make_import_key(0, 50)] = HLE_KeGetCurrentPrcb;
    hle_functions_[make_import_key(0, 49)] = HLE_KeGetCurrentProcessorNumber;
    hle_functions_[make_import_key(0, 280)] = HLE_KeTlsAlloc;
    hle_functions_[make_import_key(0, 281)] = HLE_KeTlsFree;
    hle_functions_[make_import_key(0, 282)] = HLE_KeTlsGetValue;
    hle_functions_[make_import_key(0, 283)] = HLE_KeTlsSetValue;
    hle_functions_[make_import_key(0, 78)] = HLE_KeSetBasePriorityThread;
    hle_functions_[make_import_key(0, 79)] = HLE_KeSetDisableBoostThread;
    hle_functions_[make_import_key(0, 75)] = HLE_KeResumeThread;
    hle_functions_[make_import_key(0, 84)] = HLE_KeSuspendThread;
    
    // Time
    hle_functions_[make_import_key(0, 105)] = HLE_KeQuerySystemTime;
    hle_functions_[make_import_key(0, 104)] = HLE_KeQueryInterruptTime;
    hle_functions_[make_import_key(0, 35)] = HLE_NtQuerySystemTime;
    hle_functions_[make_import_key(0, 252)] = HLE_RtlTimeToTimeFields;
    
    // Interlocked
    hle_functions_[make_import_key(0, 46)] = HLE_InterlockedIncrement;
    hle_functions_[make_import_key(0, 45)] = HLE_InterlockedDecrement;
    hle_functions_[make_import_key(0, 44)] = HLE_InterlockedExchange;
    hle_functions_[make_import_key(0, 43)] = HLE_InterlockedCompareExchange;
    hle_functions_[make_import_key(0, 42)] = HLE_InterlockedExchangeAdd;
    
    // Object
    hle_functions_[make_import_key(0, 140)] = HLE_ObReferenceObjectByHandle;
    hle_functions_[make_import_key(0, 141)] = HLE_ObDereferenceObject;
    hle_functions_[make_import_key(0, 138)] = HLE_ObCreateObject;
    
    // Exception
    hle_functions_[make_import_key(0, 255)] = HLE_RtlUnwind;
    hle_functions_[make_import_key(0, 171)] = HLE_RtlCaptureContext;
    hle_functions_[make_import_key(0, 196)] = HLE_RtlLookupFunctionEntry;
    hle_functions_[make_import_key(0, 257)] = HLE_RtlVirtualUnwind;
    
    // String
    hle_functions_[make_import_key(0, 174)] = HLE_RtlCompareString;
    hle_functions_[make_import_key(0, 175)] = HLE_RtlCompareStringN;
    hle_functions_[make_import_key(0, 176)] = HLE_RtlCopyString;
    hle_functions_[make_import_key(0, 256)] = HLE_RtlUnicodeStringToAnsiString;
    hle_functions_[make_import_key(0, 167)] = HLE_RtlAnsiStringToUnicodeString;
    hle_functions_[make_import_key(0, 178)] = HLE_RtlFreeAnsiString;
    hle_functions_[make_import_key(0, 179)] = HLE_RtlFreeUnicodeString;
    
    // Random
    hle_functions_[make_import_key(0, 218)] = HLE_RtlRandom;
    hle_functions_[make_import_key(0, 219)] = HLE_RtlRandomEx;
    
    // System
    hle_functions_[make_import_key(0, 290)] = HLE_XeKeysGetKey;
    hle_functions_[make_import_key(0, 405)] = HLE_XexGetModuleHandle;
    hle_functions_[make_import_key(0, 406)] = HLE_XexGetModuleSection;
    hle_functions_[make_import_key(0, 407)] = HLE_XexGetProcedureAddress;
    hle_functions_[make_import_key(0, 27)] = HLE_HalReturnToFirmware;
    hle_functions_[make_import_key(0, 336)] = HLE_KeBugCheck;
    hle_functions_[make_import_key(0, 337)] = HLE_KeBugCheckEx;
    
    // DPC
    hle_functions_[make_import_key(0, 57)] = HLE_KeInitializeDpc;
    hle_functions_[make_import_key(0, 59)] = HLE_KeInsertQueueDpc;
    hle_functions_[make_import_key(0, 74)] = HLE_KeRemoveQueueDpc;
    
    // Timer
    hle_functions_[make_import_key(0, 63)] = HLE_KeInitializeTimerEx;
    hle_functions_[make_import_key(0, 83)] = HLE_KeSetTimerEx;
    hle_functions_[make_import_key(0, 38)] = HLE_KeCancelTimer;
    
    // APC
    hle_functions_[make_import_key(0, 54)] = HLE_KeInitializeApc;
    hle_functions_[make_import_key(0, 61)] = HLE_KeInsertQueueApc;
    
    LOGI("Registered extended xboxkrnl.exe HLE functions (total: %zu)", hle_functions_.size());
}

} // namespace x360mu

