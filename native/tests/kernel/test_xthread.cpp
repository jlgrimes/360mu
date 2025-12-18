/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XThread and XScheduler Unit Tests
 */

#include <gtest/gtest.h>
#include "kernel/xthread.h"
#include "kernel/xobject.h"
#include "kernel/xevent.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include <thread>
#include <chrono>

namespace x360mu {
namespace test {

//=============================================================================
// XThread Basic Tests
//=============================================================================

class XThreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        KernelState::instance().initialize(memory_.get());
        XScheduler::instance().initialize(cpu_.get(), memory_.get());
    }
    
    void TearDown() override {
        XScheduler::instance().shutdown();
        KernelState::instance().shutdown();
        cpu_->shutdown();
        memory_->shutdown();
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
};

TEST_F(XThreadTest, CreateThread) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,  // Entry point
        0x12345678,  // Parameter
        64 * 1024,   // Stack size
        0,           // Flags
        false        // Not system thread
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->type(), XObjectType::Thread);
    EXPECT_NE(thread->thread_id(), 0u);
    EXPECT_EQ(thread->entry_point(), 0x82000000u);
    EXPECT_EQ(thread->state(), XThreadState::Ready);
}

TEST_F(XThreadTest, CreateSuspendedThread) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0x04,  // CREATE_SUSPENDED
        false
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->state(), XThreadState::Suspended);
}

TEST_F(XThreadTest, CreateSystemThread) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        true  // System thread
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_TRUE(thread->is_system_thread());
}

TEST_F(XThreadTest, ThreadHasStack) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        128 * 1024,  // 128KB stack
        0,
        false
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_NE(thread->stack_base(), 0u);
    EXPECT_GE(thread->stack_size(), 128u * 1024u);
}

TEST_F(XThreadTest, ThreadHasTls) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_NE(thread->tls_address(), 0u);
}

TEST_F(XThreadTest, ThreadHasGuestStruct) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_NE(thread->guest_thread(), 0u);
    
    // Check guest structure was initialized
    u8 type = memory_->read_u8(thread->guest_thread());
    EXPECT_EQ(type, static_cast<u8>(XObjectType::Thread));
}

TEST_F(XThreadTest, ThreadIdUnique) {
    std::vector<u32> ids;
    
    for (int i = 0; i < 10; i++) {
        auto thread = XThread::create(
            cpu_.get(),
            memory_.get(),
            0x82000000,
            0,
            64 * 1024,
            0x04,  // Suspended to avoid running
            false
        );
        ASSERT_NE(thread, nullptr);
        ids.push_back(thread->thread_id());
    }
    
    // All IDs should be unique
    std::set<u32> unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(unique_ids.size(), 10u);
}

TEST_F(XThreadTest, SetPriority) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    EXPECT_EQ(thread->priority(), XThreadPriority::Normal);
    
    thread->set_priority(XThreadPriority::Highest);
    EXPECT_EQ(thread->priority(), XThreadPriority::Highest);
    
    thread->set_priority(XThreadPriority::Lowest);
    EXPECT_EQ(thread->priority(), XThreadPriority::Lowest);
}

TEST_F(XThreadTest, SetAffinity) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    // Default should be all cores
    EXPECT_EQ(thread->affinity_mask(), XThreadAffinity::AllCores);
    
    // Set to specific core
    thread->set_affinity(Core0Thread0);
    EXPECT_EQ(thread->affinity_mask(), Core0Thread0);
    
    // Set to invalid (0) should default to AllCores
    thread->set_affinity(0);
    EXPECT_EQ(thread->affinity_mask(), XThreadAffinity::AllCores);
}

TEST_F(XThreadTest, SuspendResume) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    EXPECT_EQ(thread->state(), XThreadState::Ready);
    
    thread->suspend();
    EXPECT_EQ(thread->state(), XThreadState::Suspended);
    
    thread->resume();
    EXPECT_EQ(thread->state(), XThreadState::Ready);
}

TEST_F(XThreadTest, Terminate) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    thread->terminate(42);
    
    EXPECT_EQ(thread->state(), XThreadState::Terminated);
    EXPECT_EQ(thread->exit_code(), 42u);
    EXPECT_TRUE(thread->is_terminated());
}

TEST_F(XThreadTest, ThreadSignaledWhenTerminated) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    EXPECT_FALSE(thread->is_signaled());  // Running thread not signaled
    
    thread->terminate(0);
    
    EXPECT_TRUE(thread->is_signaled());  // Terminated thread is signaled
}

TEST_F(XThreadTest, QueueApc) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0x04,  // Suspended
        false
    );
    
    // Queue some APCs
    thread->queue_apc(0x82001000, 0x11111111);
    thread->queue_apc(0x82002000, 0x22222222);
    
    // Deliver them (should not crash)
    thread->deliver_apcs();
}

//=============================================================================
// XScheduler Tests
//=============================================================================

TEST_F(XThreadTest, SchedulerAddThread) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    XScheduler::instance().add_thread(thread);
    
    auto found = XScheduler::instance().get_thread(thread->thread_id());
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), thread.get());
}

TEST_F(XThreadTest, SchedulerRemoveThread) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    XScheduler::instance().add_thread(thread);
    XScheduler::instance().remove_thread(thread.get());
    
    auto found = XScheduler::instance().get_thread(thread->thread_id());
    EXPECT_EQ(found, nullptr);
}

TEST_F(XThreadTest, SchedulerGetThreadNotFound) {
    auto found = XScheduler::instance().get_thread(0xDEADBEEF);
    EXPECT_EQ(found, nullptr);
}

TEST_F(XThreadTest, SchedulerRunNoThreads) {
    // Should not crash when no threads
    XScheduler::instance().run_for(1000);
}

TEST_F(XThreadTest, SchedulerAdvanceTime) {
    u64 time_before = XScheduler::instance().current_time();
    
    XScheduler::instance().run_for(32000);  // Run for ~1ms worth of cycles
    
    u64 time_after = XScheduler::instance().current_time();
    EXPECT_GT(time_after, time_before);
}

//=============================================================================
// Wait Tests
//=============================================================================

TEST_F(XThreadTest, WaitForSignaledEvent) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    auto event = std::make_shared<XEvent>(XEventType::NotificationEvent, true);  // Signaled
    
    u32 result = thread->wait(event.get(), 0);  // 0 timeout = poll
    EXPECT_EQ(result, WAIT_OBJECT_0);
}

TEST_F(XThreadTest, WaitForUnsignaledEventTimeout) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    auto event = std::make_shared<XEvent>(XEventType::NotificationEvent, false);  // Not signaled
    
    u32 result = thread->wait(event.get(), 0);  // 0 timeout = poll
    EXPECT_EQ(result, WAIT_TIMEOUT);
}

TEST_F(XThreadTest, WaitForNullObject) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    u32 result = thread->wait(nullptr, 0);
    EXPECT_EQ(result, WAIT_FAILED);
}

TEST_F(XThreadTest, Delay) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    auto start = std::chrono::steady_clock::now();
    
    // Delay for 10ms (100000 * 100ns)
    thread->delay(100000, false);
    
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    
    // Should have delayed at least 5ms (with some margin for scheduling)
    EXPECT_GE(ms, 5);
}

//=============================================================================
// Waiter List Tests
//=============================================================================

TEST_F(XThreadTest, EventWaiterList) {
    auto event = std::make_shared<XEvent>(XEventType::NotificationEvent, false);
    
    // Create multiple threads and add as waiters
    std::vector<std::shared_ptr<XThread>> threads;
    for (int i = 0; i < 5; i++) {
        auto thread = XThread::create(
            cpu_.get(),
            memory_.get(),
            0x82000000 + i,
            0,
            64 * 1024,
            0,
            false
        );
        threads.push_back(thread);
        event->add_waiter(thread.get());
    }
    
    // Wake all waiters
    event->wake_waiters();
    
    // Should not crash, waiters should be cleared
}

TEST_F(XThreadTest, RemoveWaiter) {
    auto event = std::make_shared<XEvent>(XEventType::NotificationEvent, false);
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0,
        false
    );
    
    event->add_waiter(thread.get());
    event->remove_waiter(thread.get());
    
    // Should not crash when waking (no waiters)
    event->wake_waiters();
}

//=============================================================================
// Thread States Integration
//=============================================================================

TEST_F(XThreadTest, StateTransitions) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000000,
        0,
        64 * 1024,
        0x04,  // Created suspended
        false
    );
    
    // Created -> Suspended (via CREATE_SUSPENDED flag)
    EXPECT_EQ(thread->state(), XThreadState::Suspended);
    
    // Suspended -> Ready
    thread->resume();
    EXPECT_EQ(thread->state(), XThreadState::Ready);
    
    // Ready -> Suspended
    thread->suspend();
    EXPECT_EQ(thread->state(), XThreadState::Suspended);
    
    // Suspended -> Terminated
    thread->terminate(0);
    EXPECT_EQ(thread->state(), XThreadState::Terminated);
    
    // Once terminated, can't resume
    thread->resume();
    EXPECT_EQ(thread->state(), XThreadState::Terminated);
}

//=============================================================================
// CPU Context Tests
//=============================================================================

TEST_F(XThreadTest, CpuContextInitialized) {
    auto thread = XThread::create(
        cpu_.get(),
        memory_.get(),
        0x82000100,   // Entry point
        0xDEADBEEF,   // Parameter
        64 * 1024,
        0,
        false
    );
    
    auto& ctx = cpu_->get_context(thread->cpu_thread_id());
    
    EXPECT_EQ(ctx.pc, 0x82000100u);       // Entry point
    EXPECT_EQ(ctx.gpr[3], 0xDEADBEEFu);   // Parameter in r3
    EXPECT_EQ(ctx.gpr[13], thread->tls_address());  // TLS pointer in r13
    EXPECT_NE(ctx.gpr[1], 0u);            // Stack pointer
}

} // namespace test
} // namespace x360mu
