/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Syscall Integration Tests
 * 
 * Tests the full path: CPU → syscall dispatch → HLE function → result
 */

#include <gtest/gtest.h>
#include "kernel/kernel.h"
#include "kernel/threading.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "cpu/xenon/threading.h"

namespace x360mu {
namespace test {

// Status codes
namespace nt {
constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_NO_MEMORY = 0xC0000017;
constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;
}

// Memory allocation flags
constexpr u32 MEM_COMMIT = 0x1000;
constexpr u32 MEM_RESERVE = 0x2000;
constexpr u32 PAGE_READWRITE = 0x04;

class SyscallIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        scheduler_ = std::make_unique<ThreadScheduler>();
        // Use 0 host threads for deterministic testing
        ASSERT_EQ(scheduler_->initialize(memory_.get(), nullptr, cpu_.get(), 0), Status::Ok);
        
        kernel_ = std::make_unique<Kernel>();
        ASSERT_EQ(kernel_->initialize(memory_.get(), cpu_.get(), nullptr), Status::Ok);
        kernel_->set_scheduler(scheduler_.get());
        cpu_->set_kernel(kernel_.get());
        
        thread_mgr_ = std::make_unique<KernelThreadManager>();
        ASSERT_EQ(thread_mgr_->initialize(memory_.get(), cpu_.get(), scheduler_.get()), Status::Ok);
        set_kernel_thread_manager(thread_mgr_.get());
    }
    
    void TearDown() override {
        set_kernel_thread_manager(nullptr);
        thread_mgr_->shutdown();
        kernel_->shutdown();
        scheduler_->shutdown();
        cpu_->shutdown();
        memory_->shutdown();
    }
    
    // Helper to call HLE function directly (simulating syscall dispatch)
    void call_hle_function(u32 ordinal, u64* args, u64* result) {
        kernel_->handle_syscall(ordinal, 0);  // Module 0 = xboxkrnl.exe
        auto& ctx = cpu_->get_context(0);
        *result = ctx.gpr[3];
    }
    
    // Setup CPU registers for a syscall
    void setup_syscall_args(u64 arg0, u64 arg1 = 0, u64 arg2 = 0, u64 arg3 = 0,
                           u64 arg4 = 0, u64 arg5 = 0, u64 arg6 = 0, u64 arg7 = 0) {
        auto& ctx = cpu_->get_context(0);
        ctx.gpr[3] = arg0;
        ctx.gpr[4] = arg1;
        ctx.gpr[5] = arg2;
        ctx.gpr[6] = arg3;
        ctx.gpr[7] = arg4;
        ctx.gpr[8] = arg5;
        ctx.gpr[9] = arg6;
        ctx.gpr[10] = arg7;
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<ThreadScheduler> scheduler_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<KernelThreadManager> thread_mgr_;
};

//=============================================================================
// NtAllocateVirtualMemory Tests
//=============================================================================

TEST_F(SyscallIntegrationTest, NtAllocateVirtualMemory_Success) {
    // Allocate memory for the in/out parameters
    GuestAddr base_addr_ptr = 0x10000;
    GuestAddr region_size_ptr = 0x10010;
    
    // Initialize parameters
    memory_->write_u32(base_addr_ptr, 0);  // Let system choose address
    memory_->write_u32(region_size_ptr, 0x10000);  // Request 64KB
    
    // Setup syscall args:
    // arg0 = ProcessHandle (-1 for current)
    // arg1 = BaseAddress pointer
    // arg2 = ZeroBits
    // arg3 = RegionSize pointer
    // arg4 = AllocationType
    // arg5 = Protect
    setup_syscall_args(
        0xFFFFFFFF,           // ProcessHandle = current process
        base_addr_ptr,        // BaseAddress ptr
        0,                    // ZeroBits
        region_size_ptr,      // RegionSize ptr
        MEM_COMMIT | MEM_RESERVE,  // AllocationType
        PAGE_READWRITE        // Protect
    );
    
    // Call NtAllocateVirtualMemory (ordinal 186)
    u64 result = 0;
    call_hle_function(186, nullptr, &result);
    
    EXPECT_EQ(result, nt::STATUS_SUCCESS);
    
    // Verify memory was allocated
    GuestAddr allocated_base = memory_->read_u32(base_addr_ptr);
    EXPECT_NE(allocated_base, 0u);
    
    u32 allocated_size = memory_->read_u32(region_size_ptr);
    EXPECT_GE(allocated_size, 0x10000u);
    
    // Verify we can write to the allocated memory
    memory_->write_u32(allocated_base, 0xDEADBEEF);
    EXPECT_EQ(memory_->read_u32(allocated_base), 0xDEADBEEFu);
}

TEST_F(SyscallIntegrationTest, NtAllocateVirtualMemory_SpecificAddress) {
    GuestAddr base_addr_ptr = 0x10000;
    GuestAddr region_size_ptr = 0x10010;
    
    // Request specific address - use a valid address within 512MB physical memory
    GuestAddr requested_addr = 0x10000000;  // 256MB - well within limits
    memory_->write_u32(base_addr_ptr, requested_addr);
    memory_->write_u32(region_size_ptr, 0x10000);
    
    setup_syscall_args(
        0xFFFFFFFF,
        base_addr_ptr,
        0,
        region_size_ptr,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    
    u64 result = 0;
    call_hle_function(186, nullptr, &result);  // NtAllocateVirtualMemory ordinal 186
    
    EXPECT_EQ(result, nt::STATUS_SUCCESS);
    
    // Should get address at or near what we requested
    GuestAddr allocated_base = memory_->read_u32(base_addr_ptr);
    // The address should be page-aligned and in a reasonable range
    EXPECT_NE(allocated_base, 0u);
}

//=============================================================================
// KeInitializeEvent / KeSetEvent / KeResetEvent Tests
//=============================================================================

TEST_F(SyscallIntegrationTest, EventLifecycle) {
    // Allocate event structure (16 bytes for dispatcher header)
    GuestAddr event_addr = 0x20000;
    
    // KeInitializeEvent: ordinal 58
    // arg0 = Event pointer
    // arg1 = Type (0 = Notification, 1 = Synchronization)
    // arg2 = InitialState
    setup_syscall_args(event_addr, 0, 0);  // NotificationEvent, not signaled
    
    u64 result = 0;
    call_hle_function(58, nullptr, &result);
    
    // Verify event structure was initialized
    u8 event_type = memory_->read_u8(event_addr);
    EXPECT_EQ(event_type, 0u);  // NotificationEvent
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);  // Not signaled
    
    // KeSetEvent: ordinal 82
    // arg0 = Event pointer
    // arg1 = Increment
    // arg2 = Wait
    setup_syscall_args(event_addr, 0, 0);
    call_hle_function(82, nullptr, &result);
    
    // Verify event is now signaled
    signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 1u);
    
    // KeResetEvent: ordinal 77
    setup_syscall_args(event_addr);
    call_hle_function(77, nullptr, &result);
    
    // Verify event is reset
    signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);
}

//=============================================================================
// KeQueryPerformanceCounter Tests
//=============================================================================

TEST_F(SyscallIntegrationTest, QueryPerformanceCounter) {
    // KeQueryPerformanceCounter: ordinal 102
    // Returns 64-bit counter value
    
    setup_syscall_args(0);
    
    u64 result1 = 0;
    call_hle_function(102, nullptr, &result1);
    
    // Counter should be non-zero and increasing
    EXPECT_GT(result1, 0u);
    
    // Call again - should be equal or greater
    u64 result2 = 0;
    call_hle_function(102, nullptr, &result2);
    EXPECT_GE(result2, result1);
}

TEST_F(SyscallIntegrationTest, QueryPerformanceFrequency) {
    // KeQueryPerformanceFrequency: ordinal 103
    // Returns frequency (should be ~50MHz for Xbox 360)
    
    setup_syscall_args(0);
    
    u64 result = 0;
    call_hle_function(103, nullptr, &result);
    
    // Xbox 360 timer frequency is ~50MHz
    EXPECT_GT(result, 1000000u);  // At least 1MHz
}

//=============================================================================
// String Functions Tests
//=============================================================================

TEST_F(SyscallIntegrationTest, RtlInitAnsiString) {
    // RtlInitAnsiString: ordinal 183
    // Initializes an ANSI_STRING structure
    
    // Setup string buffer
    GuestAddr string_addr = 0x30000;
    const char* test_str = "Hello, Xbox!";
    memory_->write_bytes(string_addr, test_str, strlen(test_str) + 1);
    
    // Setup ANSI_STRING structure
    GuestAddr ansi_string_addr = 0x30100;
    memory_->write_u16(ansi_string_addr, 0);      // Length
    memory_->write_u16(ansi_string_addr + 2, 0);  // MaxLength
    memory_->write_u32(ansi_string_addr + 4, 0);  // Buffer
    
    setup_syscall_args(ansi_string_addr, string_addr);
    
    u64 result = 0;
    call_hle_function(276, nullptr, &result);  // RtlInitAnsiString ordinal 276
    
    // Verify structure was filled in
    u16 length = memory_->read_u16(ansi_string_addr);
    EXPECT_EQ(length, strlen(test_str));
    
    u16 max_length = memory_->read_u16(ansi_string_addr + 2);
    EXPECT_EQ(max_length, strlen(test_str) + 1);
    
    GuestAddr buffer = memory_->read_u32(ansi_string_addr + 4);
    EXPECT_EQ(buffer, string_addr);
}

//=============================================================================
// Semaphore Tests
//=============================================================================

TEST_F(SyscallIntegrationTest, SemaphoreInitAndRelease) {
    // KeInitializeSemaphore: ordinal 60
    GuestAddr sem_addr = 0x40000;
    
    // arg0 = Semaphore ptr
    // arg1 = Count (initial)
    // arg2 = Limit (max)
    setup_syscall_args(sem_addr, 2, 10);  // Initial=2, Max=10
    
    u64 result = 0;
    call_hle_function(60, nullptr, &result);
    
    // Verify structure - semaphore type is 5
    u8 type = memory_->read_u8(sem_addr);
    EXPECT_EQ(type, 5u);
    
    u32 signal_state = memory_->read_u32(sem_addr + 4);
    EXPECT_EQ(signal_state, 2u);  // Initial count
    
    // KeReleaseSemaphore: ordinal 108
    // arg0 = Semaphore ptr
    // arg1 = Increment
    // arg2 = Wait
    setup_syscall_args(sem_addr, 3, 0);  // Release 3
    call_hle_function(108, nullptr, &result);
    
    // Result should be previous count (2)
    EXPECT_EQ(result, 2u);
    
    // New count should be 5 (was 2, released 3)
    signal_state = memory_->read_u32(sem_addr + 4);
    EXPECT_EQ(signal_state, 5u);
}

} // namespace test
} // namespace x360mu
