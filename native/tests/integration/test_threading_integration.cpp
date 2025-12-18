/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Threading Integration Tests
 * 
 * Tests multi-threaded scenarios that simulate real game behavior,
 * particularly the main-thread-waits-for-worker pattern that caused
 * Call of Duty: Black Ops to hang.
 */

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include "kernel/kernel.h"
#include "kernel/threading.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "cpu/xenon/threading.h"

namespace x360mu {
namespace test {

namespace nt {
constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_TIMEOUT = 0x00000102;
constexpr u32 STATUS_WAIT_0 = 0x00000000;
}

class ThreadingIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        // Use multiple host threads for realistic testing
        scheduler_ = std::make_unique<ThreadScheduler>();
        ASSERT_EQ(scheduler_->initialize(memory_.get(), nullptr, cpu_.get(), 2), Status::Ok);
        
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
    
    // Helper to create and initialize an event in guest memory
    u32 create_event(bool notification_type, bool initial_state) {
        u32 handle = 0;
        EventType type = notification_type ? EventType::NotificationEvent 
                                           : EventType::SynchronizationEvent;
        thread_mgr_->create_event(&handle, 0, 0, type, initial_state);
        return handle;
    }
    
    // Helper to create a guest thread
    u32 create_guest_thread(GuestAddr entry_point, bool suspended = false) {
        u32 handle = 0;
        u32 thread_id = 0;
        u32 flags = suspended ? ::x360mu::nt::CREATE_SUSPENDED : 0;
        thread_mgr_->create_thread(&handle, 64 * 1024, &thread_id, 0, entry_point, 0, flags);
        return handle;
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<ThreadScheduler> scheduler_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<KernelThreadManager> thread_mgr_;
};

//=============================================================================
// Event Signaling Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, EventSetAndCheck) {
    // Create notification event (stays signaled until manually reset)
    u32 event_handle = create_event(true, false);
    ASSERT_NE(event_handle, 0u);
    
    // Initially not signaled - wait should timeout
    s64 short_timeout = 0;  // No wait, just check
    u32 status = thread_mgr_->wait_for_single_object(event_handle, false, &short_timeout);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
    
    // Set the event
    s32 prev_state = 0;
    thread_mgr_->set_event(event_handle, &prev_state);
    
    // Now should be signaled
    status = thread_mgr_->wait_for_single_object(event_handle, false, &short_timeout);
    EXPECT_EQ(status, nt::STATUS_WAIT_0);
    
    thread_mgr_->close_handle(event_handle);
}

TEST_F(ThreadingIntegrationTest, SynchronizationEventAutoReset) {
    // Create synchronization event (auto-resets after one waiter satisfied)
    u32 event_handle = create_event(false, true);  // Initially signaled
    ASSERT_NE(event_handle, 0u);
    
    s64 no_wait = 0;
    
    // First wait should succeed and auto-reset
    u32 status1 = thread_mgr_->wait_for_single_object(event_handle, false, &no_wait);
    EXPECT_EQ(status1, nt::STATUS_WAIT_0);
    
    // Second wait should timeout (event auto-reset)
    u32 status2 = thread_mgr_->wait_for_single_object(event_handle, false, &no_wait);
    EXPECT_EQ(status2, nt::STATUS_TIMEOUT);
    
    thread_mgr_->close_handle(event_handle);
}

//=============================================================================
// Multi-Thread Coordination Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, ThreadCreationAndScheduling) {
    // Create multiple threads
    u32 handle1 = create_guest_thread(0x82000000);
    u32 handle2 = create_guest_thread(0x82001000);
    u32 handle3 = create_guest_thread(0x82002000);
    
    EXPECT_NE(handle1, 0u);
    EXPECT_NE(handle2, 0u);
    EXPECT_NE(handle3, 0u);
    
    // All handles should be unique
    EXPECT_NE(handle1, handle2);
    EXPECT_NE(handle2, handle3);
    EXPECT_NE(handle1, handle3);
    
    // Get thread objects
    GuestThread* t1 = scheduler_->get_thread_by_handle(handle1);
    GuestThread* t2 = scheduler_->get_thread_by_handle(handle2);
    GuestThread* t3 = scheduler_->get_thread_by_handle(handle3);
    
    EXPECT_NE(t1, nullptr);
    EXPECT_NE(t2, nullptr);
    EXPECT_NE(t3, nullptr);
    
    // Run scheduler to pick up threads
    for (int i = 0; i < 10; i++) {
        scheduler_->run(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // At least one thread should have been scheduled
    auto stats = scheduler_->get_stats();
    EXPECT_GT(stats.context_switches, 0u);
    
    // Cleanup
    thread_mgr_->close_handle(handle1);
    thread_mgr_->close_handle(handle2);
    thread_mgr_->close_handle(handle3);
}

TEST_F(ThreadingIntegrationTest, SuspendedThreadNotScheduled) {
    // Create a suspended thread
    u32 handle = create_guest_thread(0x82000000, true);
    ASSERT_NE(handle, 0u);
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    ASSERT_NE(thread, nullptr);
    
    // Thread should be suspended
    EXPECT_EQ(thread->state, ThreadState::Suspended);
    EXPECT_EQ(thread->suspend_count, 1u);
    
    // Run scheduler
    scheduler_->run(1000);
    
    // Thread should still be suspended, not running
    EXPECT_EQ(thread->state, ThreadState::Suspended);
    
    // Resume the thread
    u32 prev_count = 0;
    thread_mgr_->resume_thread(handle, &prev_count);
    EXPECT_EQ(prev_count, 1u);
    
    // Now thread should be ready
    EXPECT_EQ(thread->state, ThreadState::Ready);
    
    thread_mgr_->close_handle(handle);
}

//=============================================================================
// Semaphore Coordination Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, SemaphoreResourceCounting) {
    // Create semaphore with 3 resources, max 5
    u32 sem_handle = 0;
    thread_mgr_->create_semaphore(&sem_handle, 0, 0, 3, 5);
    ASSERT_NE(sem_handle, 0u);
    
    s64 no_wait = 0;
    
    // Acquire 3 resources (should succeed)
    for (int i = 0; i < 3; i++) {
        u32 status = thread_mgr_->wait_for_single_object(sem_handle, false, &no_wait);
        EXPECT_EQ(status, nt::STATUS_WAIT_0) << "Acquire " << i << " failed";
    }
    
    // 4th acquire should timeout (no resources left)
    u32 status = thread_mgr_->wait_for_single_object(sem_handle, false, &no_wait);
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
    
    // Release 2 resources
    s32 prev_count = 0;
    thread_mgr_->release_semaphore(sem_handle, 2, &prev_count);
    EXPECT_EQ(prev_count, 0);  // Was empty
    
    // Now can acquire 2 more
    for (int i = 0; i < 2; i++) {
        status = thread_mgr_->wait_for_single_object(sem_handle, false, &no_wait);
        EXPECT_EQ(status, nt::STATUS_WAIT_0);
    }
    
    thread_mgr_->close_handle(sem_handle);
}

//=============================================================================
// Critical Section Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, CriticalSectionMutualExclusion) {
    // Allocate critical section in guest memory
    GuestAddr cs_addr = 0x50000;
    
    // Initialize critical section
    thread_mgr_->init_critical_section(cs_addr);
    
    // Enter (should succeed - no contention)
    u32 status = thread_mgr_->enter_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Recursive enter (should succeed)
    status = thread_mgr_->enter_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    // Leave twice
    status = thread_mgr_->leave_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
    
    status = thread_mgr_->leave_critical_section(cs_addr);
    EXPECT_EQ(status, nt::STATUS_SUCCESS);
}

TEST_F(ThreadingIntegrationTest, TryEnterCriticalSection) {
    GuestAddr cs_addr = 0x50000;
    thread_mgr_->init_critical_section(cs_addr);
    
    // Try enter should succeed (not held)
    u32 result = thread_mgr_->try_enter_critical_section(cs_addr);
    EXPECT_EQ(result, 1u);  // TRUE = success
    
    // Another try enter should succeed (recursive)
    result = thread_mgr_->try_enter_critical_section(cs_addr);
    EXPECT_EQ(result, 1u);
    
    // Leave both
    thread_mgr_->leave_critical_section(cs_addr);
    thread_mgr_->leave_critical_section(cs_addr);
}

//=============================================================================
// Timeout Behavior Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, WaitWithTimeout) {
    // Create unsignaled event
    u32 event_handle = create_event(true, false);
    ASSERT_NE(event_handle, 0u);
    
    // Wait with 10ms timeout (should timeout)
    s64 timeout_100ns = -100000;  // -10ms in 100ns units (negative = relative)
    
    auto start = std::chrono::steady_clock::now();
    u32 status = thread_mgr_->wait_for_single_object(event_handle, false, &timeout_100ns);
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_EQ(status, nt::STATUS_TIMEOUT);
    
    // Should have waited approximately the timeout duration
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_GE(elapsed_ms, 5);  // At least 5ms (allowing some slack)
    EXPECT_LT(elapsed_ms, 1000);  // But not forever
    
    thread_mgr_->close_handle(event_handle);
}

TEST_F(ThreadingIntegrationTest, WaitNoTimeout) {
    // Create signaled event
    u32 event_handle = create_event(true, true);
    ASSERT_NE(event_handle, 0u);
    
    // Wait should return immediately
    s64 timeout = -1000000000LL;  // 100 second timeout
    
    auto start = std::chrono::steady_clock::now();
    u32 status = thread_mgr_->wait_for_single_object(event_handle, false, &timeout);
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_EQ(status, nt::STATUS_WAIT_0);
    
    // Should have returned quickly (not waited)
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(elapsed_ms, 100);  // Should be fast
    
    thread_mgr_->close_handle(event_handle);
}

//=============================================================================
// Thread Priority Tests  
//=============================================================================

TEST_F(ThreadingIntegrationTest, ThreadPriorityAffectsScheduling) {
    // Create threads with different priorities
    u32 handle_low = create_guest_thread(0x82000000);
    u32 handle_high = create_guest_thread(0x82001000);
    
    GuestThread* low_thread = scheduler_->get_thread_by_handle(handle_low);
    GuestThread* high_thread = scheduler_->get_thread_by_handle(handle_high);
    
    ASSERT_NE(low_thread, nullptr);
    ASSERT_NE(high_thread, nullptr);
    
    // Set priorities (Lowest = -2, Highest = 2)
    scheduler_->set_priority(low_thread, ThreadPriority::Lowest);
    scheduler_->set_priority(high_thread, ThreadPriority::Highest);
    
    EXPECT_EQ(low_thread->priority, ThreadPriority::Lowest);
    EXPECT_EQ(high_thread->priority, ThreadPriority::Highest);
    
    thread_mgr_->close_handle(handle_low);
    thread_mgr_->close_handle(handle_high);
}

//=============================================================================
// Scheduler Statistics Tests
//=============================================================================

TEST_F(ThreadingIntegrationTest, SchedulerTracksStatistics) {
    auto initial_stats = scheduler_->get_stats();
    u32 initial_threads = initial_stats.total_threads_created;
    
    // Create some threads
    u32 h1 = create_guest_thread(0x82000000);
    u32 h2 = create_guest_thread(0x82001000);
    
    auto stats = scheduler_->get_stats();
    EXPECT_EQ(stats.total_threads_created, initial_threads + 2);
    EXPECT_GE(stats.ready_thread_count, 0u);
    
    thread_mgr_->close_handle(h1);
    thread_mgr_->close_handle(h2);
}

} // namespace test
} // namespace x360mu
