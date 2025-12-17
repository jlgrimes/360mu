/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Threading & Synchronization Unit Tests
 */

#include <gtest/gtest.h>
#include "kernel/threading.h"
#include "kernel/kernel.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "cpu/xenon/threading.h"

namespace x360mu {
namespace test {

class ThreadingTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        ASSERT_EQ(cpu_->initialize(memory_.get()), Status::Ok);
        
        scheduler_ = std::make_unique<ThreadScheduler>();
        ASSERT_EQ(scheduler_->initialize(memory_.get(), nullptr, 1), Status::Ok);
        
        thread_mgr_ = std::make_unique<KernelThreadManager>();
        ASSERT_EQ(thread_mgr_->initialize(memory_.get(), cpu_.get(), scheduler_.get()), Status::Ok);
        
        set_kernel_thread_manager(thread_mgr_.get());
    }
    
    void TearDown() override {
        set_kernel_thread_manager(nullptr);
        thread_mgr_->shutdown();
        scheduler_->shutdown();
        cpu_->shutdown();
        memory_->shutdown();
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<ThreadScheduler> scheduler_;
    std::unique_ptr<KernelThreadManager> thread_mgr_;
};

//=============================================================================
// Thread Creation Tests
//=============================================================================

TEST_F(ThreadingTest, CreateThread) {
    u32 handle = 0;
    u32 thread_id = 0;
    
    // Create a thread
    u32 status = thread_mgr_->create_thread(
        &handle,
        64 * 1024,      // Stack size
        &thread_id,
        0,              // XAPI startup (none)
        0x82000000,     // Entry point
        0x12345678,     // Parameter
        0               // Flags (not suspended)
    );
    
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
    EXPECT_NE(thread_id, 0u);
}

TEST_F(ThreadingTest, CreateSuspendedThread) {
    u32 handle = 0;
    u32 thread_id = 0;
    
    // Create a suspended thread
    u32 status = thread_mgr_->create_thread(
        &handle,
        64 * 1024,
        &thread_id,
        0,
        0x82000000,
        0,
        nt::CREATE_SUSPENDED
    );
    
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
    
    // Thread should be suspended
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->state, ThreadState::Suspended);
    EXPECT_EQ(thread->suspend_count, 1u);
}

TEST_F(ThreadingTest, SuspendResumeThread) {
    u32 handle = 0;
    u32 thread_id = 0;
    
    // Create a running thread
    thread_mgr_->create_thread(&handle, 64 * 1024, &thread_id, 0, 0x82000000, 0, 0);
    
    // Suspend it
    u32 prev_count = 0;
    u32 status = thread_mgr_->suspend_thread(handle, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_count, 0u);
    
    // Suspend again
    status = thread_mgr_->suspend_thread(handle, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_count, 1u);
    
    // Resume once
    status = thread_mgr_->resume_thread(handle, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_count, 2u);
    
    // Resume again (should fully resume)
    status = thread_mgr_->resume_thread(handle, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_count, 1u);
}

//=============================================================================
// Event Tests
//=============================================================================

TEST_F(ThreadingTest, CreateNotificationEvent) {
    u32 handle = 0;
    
    u32 status = thread_mgr_->create_event(
        &handle,
        0,
        0,
        EventType::NotificationEvent,
        false  // Not initially signaled
    );
    
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
}

TEST_F(ThreadingTest, CreateSynchronizationEvent) {
    u32 handle = 0;
    
    u32 status = thread_mgr_->create_event(
        &handle,
        0,
        0,
        EventType::SynchronizationEvent,
        true  // Initially signaled
    );
    
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
}

TEST_F(ThreadingTest, SetClearEvent) {
    u32 handle = 0;
    thread_mgr_->create_event(&handle, 0, 0, EventType::NotificationEvent, false);
    
    // Event starts not signaled, wait should timeout immediately with 0 timeout
    s64 zero_timeout = 0;
    u32 status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
    
    // Set the event
    s32 prev_state = 0;
    status = thread_mgr_->set_event(handle, &prev_state);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_state, 0);  // Was not signaled
    
    // Now wait should succeed
    zero_timeout = 0;
    status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_WAIT_0);
    
    // Clear the event
    status = thread_mgr_->clear_event(handle);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Wait should timeout again
    zero_timeout = 0;
    status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
}

TEST_F(ThreadingTest, SynchronizationEventAutoReset) {
    u32 handle = 0;
    
    // Create initially signaled synchronization event
    thread_mgr_->create_event(&handle, 0, 0, EventType::SynchronizationEvent, true);
    
    // First wait should succeed and auto-reset the event
    s64 zero_timeout = 0;
    u32 status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_WAIT_0);
    
    // Second wait should timeout (event was auto-reset)
    zero_timeout = 0;
    status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
}

TEST_F(ThreadingTest, PulseEvent) {
    u32 handle = 0;
    thread_mgr_->create_event(&handle, 0, 0, EventType::NotificationEvent, false);
    
    // Pulse sets then immediately resets
    s32 prev_state = 0;
    u32 status = thread_mgr_->pulse_event(handle, &prev_state);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_state, 0);  // Was not signaled
    
    // Event should be reset after pulse
    s64 zero_timeout = 0;
    status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
}

//=============================================================================
// Semaphore Tests
//=============================================================================

TEST_F(ThreadingTest, CreateSemaphore) {
    u32 handle = 0;
    
    u32 status = thread_mgr_->create_semaphore(&handle, 0, 0, 0, 10);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
}

TEST_F(ThreadingTest, SemaphoreCountZero) {
    u32 handle = 0;
    thread_mgr_->create_semaphore(&handle, 0, 0, 0, 10);  // Count = 0
    
    // Wait should timeout (count is 0)
    s64 zero_timeout = 0;
    u32 status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
}

TEST_F(ThreadingTest, SemaphoreRelease) {
    u32 handle = 0;
    thread_mgr_->create_semaphore(&handle, 0, 0, 0, 10);  // Count = 0, max = 10
    
    // Release 3
    s32 prev_count = 0;
    u32 status = thread_mgr_->release_semaphore(handle, 3, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_EQ(prev_count, 0);
    
    // Wait should succeed 3 times
    for (int i = 0; i < 3; i++) {
        s64 zero_timeout = 0;
        status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
        EXPECT_EQ(status, nt::STATUS_WAIT_0);
    }
    
    // 4th wait should timeout
    s64 zero_timeout = 0;
    status = thread_mgr_->wait_for_single_object(handle, false, &zero_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
}

TEST_F(ThreadingTest, SemaphoreMaxCount) {
    u32 handle = 0;
    thread_mgr_->create_semaphore(&handle, 0, 0, 5, 10);  // Count = 5, max = 10
    
    // Try to release more than max allows
    s32 prev_count = 0;
    u32 status = thread_mgr_->release_semaphore(handle, 10, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SEMAPHORE_LIMIT_EXCEEDED);
}

//=============================================================================
// Mutant (Mutex) Tests
//=============================================================================

TEST_F(ThreadingTest, CreateMutant) {
    u32 handle = 0;
    
    u32 status = thread_mgr_->create_mutant(&handle, 0, 0, false);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    EXPECT_NE(handle, 0u);
}

TEST_F(ThreadingTest, MutantInitialOwner) {
    u32 handle = 0;
    
    // Create with initial ownership
    u32 status = thread_mgr_->create_mutant(&handle, 0, 0, true);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Release it
    s32 prev_count = 0;
    status = thread_mgr_->release_mutant(handle, false, &prev_count);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
}

//=============================================================================
// Critical Section Tests
//=============================================================================

TEST_F(ThreadingTest, CriticalSectionInit) {
    // Allocate memory for critical section
    GuestAddr cs_addr = 0x10000000;
    memory_->allocate(cs_addr, 64, MemoryRegion::Read | MemoryRegion::Write);
    
    // Initialize
    thread_mgr_->init_critical_section(cs_addr);
    
    // Check lock count is -1 (unlocked)
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_LOCK_COUNT));
    EXPECT_EQ(lock_count, -1);
}

TEST_F(ThreadingTest, CriticalSectionEnterLeave) {
    GuestAddr cs_addr = 0x10000000;
    memory_->allocate(cs_addr, 64, MemoryRegion::Read | MemoryRegion::Write);
    
    thread_mgr_->init_critical_section(cs_addr);
    
    // Enter
    u32 status = thread_mgr_->enter_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Check lock count is 0 (locked, no waiters)
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_LOCK_COUNT));
    EXPECT_EQ(lock_count, 0);
    
    // Recursion count should be 1
    s32 recursion = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_RECURSION_COUNT));
    EXPECT_EQ(recursion, 1);
    
    // Leave
    status = thread_mgr_->leave_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Should be unlocked again
    lock_count = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_LOCK_COUNT));
    EXPECT_EQ(lock_count, -1);
}

TEST_F(ThreadingTest, CriticalSectionRecursive) {
    GuestAddr cs_addr = 0x10000000;
    memory_->allocate(cs_addr, 64, MemoryRegion::Read | MemoryRegion::Write);
    
    thread_mgr_->init_critical_section(cs_addr);
    
    // Enter twice (recursive)
    thread_mgr_->enter_critical_section(cs_addr);
    thread_mgr_->enter_critical_section(cs_addr);
    
    // Recursion count should be 2
    s32 recursion = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_RECURSION_COUNT));
    EXPECT_EQ(recursion, 2);
    
    // Leave once
    thread_mgr_->leave_critical_section(cs_addr);
    recursion = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_RECURSION_COUNT));
    EXPECT_EQ(recursion, 1);
    
    // Still locked
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_LOCK_COUNT));
    EXPECT_GE(lock_count, 0);
    
    // Leave again
    thread_mgr_->leave_critical_section(cs_addr);
    
    // Now unlocked
    lock_count = static_cast<s32>(memory_->read_u32(cs_addr + RTL_CRITICAL_SECTION_LAYOUT::OFFSET_LOCK_COUNT));
    EXPECT_EQ(lock_count, -1);
}

TEST_F(ThreadingTest, TryEnterCriticalSection) {
    GuestAddr cs_addr = 0x10000000;
    memory_->allocate(cs_addr, 64, MemoryRegion::Read | MemoryRegion::Write);
    
    thread_mgr_->init_critical_section(cs_addr);
    
    // Try to enter (should succeed)
    u32 result = thread_mgr_->try_enter_critical_section(cs_addr);
    EXPECT_NE(result, 0u);  // TRUE
    
    // Leave
    thread_mgr_->leave_critical_section(cs_addr);
}

//=============================================================================
// TLS Tests
//=============================================================================

TEST_F(ThreadingTest, TlsAllocFree) {
    u32 slot1 = thread_mgr_->tls_alloc();
    EXPECT_NE(slot1, nt::TLS_OUT_OF_INDEXES);
    EXPECT_LT(slot1, 64u);
    
    u32 slot2 = thread_mgr_->tls_alloc();
    EXPECT_NE(slot2, nt::TLS_OUT_OF_INDEXES);
    EXPECT_NE(slot1, slot2);
    
    // Free first slot
    u32 result = thread_mgr_->tls_free(slot1);
    EXPECT_NE(result, 0u);  // TRUE
    
    // Allocate again - should reuse slot1
    u32 slot3 = thread_mgr_->tls_alloc();
    EXPECT_EQ(slot3, slot1);
}

TEST_F(ThreadingTest, TlsSetGetValue) {
    u32 slot = thread_mgr_->tls_alloc();
    ASSERT_NE(slot, nt::TLS_OUT_OF_INDEXES);
    
    // Set value
    u32 result = thread_mgr_->tls_set_value(slot, 0xDEADBEEF12345678ULL);
    EXPECT_NE(result, 0u);
    
    // Get value back
    u64 value = thread_mgr_->tls_get_value(slot);
    EXPECT_EQ(value, 0xDEADBEEF12345678ULL);
}

//=============================================================================
// Wait Timeout Tests
//=============================================================================

TEST_F(ThreadingTest, WaitTimeout) {
    u32 handle = 0;
    thread_mgr_->create_event(&handle, 0, 0, EventType::NotificationEvent, false);
    
    // Wait with very short timeout (should timeout quickly)
    s64 short_timeout = -10000;  // 1ms relative (negative = relative in 100ns units)
    
    auto start = std::chrono::steady_clock::now();
    u32 status = thread_mgr_->wait_for_single_object(handle, false, &short_timeout);
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
    // Should return relatively quickly (within a reasonable margin)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

//=============================================================================
// Handle Management Tests
//=============================================================================

TEST_F(ThreadingTest, CloseHandle) {
    u32 handle = 0;
    thread_mgr_->create_event(&handle, 0, 0, EventType::NotificationEvent, false);
    
    // Close it
    u32 status = thread_mgr_->close_handle(handle);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Try to use it - should fail
    s32 prev_state;
    status = thread_mgr_->set_event(handle, &prev_state);
    EXPECT_EQ(status, nt::STATUS_INVALID_HANDLE);
}

TEST_F(ThreadingTest, InvalidHandle) {
    // Try to use invalid handle
    s32 prev_state;
    u32 status = thread_mgr_->set_event(0xDEADBEEF, &prev_state);
    EXPECT_EQ(status, nt::STATUS_INVALID_HANDLE);
}

} // namespace test
} // namespace x360mu
