/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XKernel System Unit Tests
 */

#include <gtest/gtest.h>
#include "kernel/xkernel.h"
#include "kernel/xthread.h"
#include "kernel/xevent.h"
#include "kernel/kernel.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"

namespace x360mu {
namespace test {

//=============================================================================
// XKernel Initialization Tests
//=============================================================================

class XKernelTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        // XKernel doesn't need full Kernel initialization for these tests
        // Just initialize the XKernel directly
        XKernel::instance().initialize(cpu_.get(), memory_.get(), nullptr);
    }
    
    void TearDown() override {
        XKernel::instance().shutdown();
        cpu_->shutdown();
        memory_->shutdown();
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
};

TEST_F(XKernelTest, InitializesSuccessfully) {
    // Should have set up system flags
    const auto& flags = XKernel::instance().system_flags();
    
    EXPECT_TRUE(flags.kernel_initialized);
    EXPECT_TRUE(flags.video_initialized);
    EXPECT_TRUE(flags.audio_initialized);
    EXPECT_TRUE(flags.storage_initialized);
    EXPECT_TRUE(flags.network_initialized);
    EXPECT_TRUE(flags.xam_initialized);
    EXPECT_TRUE(flags.all_ready);
}

TEST_F(XKernelTest, HasSystemProcess) {
    GuestAddr process = XKernel::instance().get_system_process();
    EXPECT_NE(process, 0u);
    
    // Check it has the right type
    u8 type = memory_->read_u8(process);
    EXPECT_EQ(type, static_cast<u8>(XObjectType::Process));
}

TEST_F(XKernelTest, HasKpcr) {
    // Should have KPCR for all 6 processors
    for (u32 i = 0; i < 6; i++) {
        GuestAddr kpcr = XKernel::instance().get_kpcr_address(i);
        EXPECT_NE(kpcr, 0u);
        
        // KPCR should have self-pointer at offset 0
        u32 self_ptr = memory_->read_u32(kpcr);
        EXPECT_EQ(self_ptr, kpcr);
    }
}

TEST_F(XKernelTest, KpcrInvalidProcessor) {
    GuestAddr kpcr = XKernel::instance().get_kpcr_address(100);  // Invalid
    EXPECT_EQ(kpcr, 0u);
}

TEST_F(XKernelTest, Accessors) {
    EXPECT_EQ(XKernel::instance().cpu(), cpu_.get());
    EXPECT_EQ(XKernel::instance().memory(), memory_.get());
    // hle_kernel is null in this test setup
    EXPECT_EQ(XKernel::instance().hle_kernel(), nullptr);
}

//=============================================================================
// Object Creation Tests
//=============================================================================

TEST_F(XKernelTest, CreateEvent) {
    auto event = XKernel::instance().create_event(XEventType::NotificationEvent, false);
    
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->event_type(), XEventType::NotificationEvent);
    EXPECT_FALSE(event->is_signaled());
    EXPECT_NE(event->handle(), 0u);
}

TEST_F(XKernelTest, CreateSemaphore) {
    auto sem = XKernel::instance().create_semaphore(5, 10);
    
    ASSERT_NE(sem, nullptr);
    EXPECT_EQ(sem->count(), 5);
    EXPECT_EQ(sem->maximum(), 10);
    EXPECT_NE(sem->handle(), 0u);
}

TEST_F(XKernelTest, CreateMutant) {
    auto mutant = XKernel::instance().create_mutant(false);
    
    ASSERT_NE(mutant, nullptr);
    EXPECT_TRUE(mutant->is_signaled());  // Not initially owned
    EXPECT_NE(mutant->handle(), 0u);
}

TEST_F(XKernelTest, CreateThread) {
    auto thread = XKernel::instance().create_thread(
        0x82000000,  // Entry
        0x12345678,  // Param
        64 * 1024,   // Stack
        0            // Flags
    );
    
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->entry_point(), 0x82000000u);
    EXPECT_NE(thread->handle(), 0u);
}

//=============================================================================
// Handle Management Tests
//=============================================================================

TEST_F(XKernelTest, CreateHandle) {
    auto event = std::make_shared<XEvent>(XEventType::NotificationEvent, false);
    
    u32 handle = XKernel::instance().create_handle(event);
    EXPECT_NE(handle, 0u);
}

TEST_F(XKernelTest, GetObject) {
    auto event = XKernel::instance().create_event(XEventType::NotificationEvent, false);
    u32 handle = event->handle();
    
    auto found = XKernel::instance().get_object(handle);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), event.get());
}

TEST_F(XKernelTest, GetObjectInvalidHandle) {
    auto found = XKernel::instance().get_object(0xDEADBEEF);
    EXPECT_EQ(found, nullptr);
}

TEST_F(XKernelTest, CloseHandle) {
    auto event = XKernel::instance().create_event(XEventType::NotificationEvent, false);
    u32 handle = event->handle();
    
    XKernel::instance().close_handle(handle);
    
    auto found = XKernel::instance().get_object(handle);
    EXPECT_EQ(found, nullptr);
}

//=============================================================================
// Event Operations Tests
//=============================================================================

TEST_F(XKernelTest, SetEventByAddr) {
    // Create event in guest memory
    GuestAddr event_addr = 0x00200000;
    
    // Initialize event header
    memory_->write_u8(event_addr, 0);  // Type: NotificationEvent
    memory_->write_u32(event_addr + 4, 0);  // SignalState: not signaled
    
    XKernel::instance().set_event(event_addr);
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 1u);  // Now signaled
}

TEST_F(XKernelTest, ResetEventByAddr) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 0);
    memory_->write_u32(event_addr + 4, 1);  // Initially signaled
    
    XKernel::instance().reset_event(event_addr);
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);  // Now not signaled
}

TEST_F(XKernelTest, PulseEventByAddr) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 0);
    memory_->write_u32(event_addr + 4, 0);  // Not signaled
    
    XKernel::instance().pulse_event(event_addr);
    
    // After pulse, should be reset
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);
}

//=============================================================================
// Wait Operations Tests
//=============================================================================

TEST_F(XKernelTest, WaitForSignaledObject) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 0);  // NotificationEvent
    memory_->write_u32(event_addr + 4, 1);  // Signaled
    
    u32 result = XKernel::instance().wait_for_single_object(event_addr, 0);
    EXPECT_EQ(result, WAIT_OBJECT_0);
}

TEST_F(XKernelTest, WaitForUnsignaledTimeout) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 0);  // NotificationEvent
    memory_->write_u32(event_addr + 4, 0);  // Not signaled
    
    u32 result = XKernel::instance().wait_for_single_object(event_addr, 0);  // Immediate timeout
    EXPECT_EQ(result, WAIT_TIMEOUT);
}

TEST_F(XKernelTest, WaitSynchronizationEventAutoReset) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 1);  // SynchronizationEvent
    memory_->write_u32(event_addr + 4, 1);  // Signaled
    
    u32 result = XKernel::instance().wait_for_single_object(event_addr, 0);
    EXPECT_EQ(result, WAIT_OBJECT_0);
    
    // Should have auto-reset
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);
}

TEST_F(XKernelTest, WaitSemaphoreDecrements) {
    GuestAddr sem_addr = 0x00200000;
    
    memory_->write_u8(sem_addr, static_cast<u8>(XObjectType::Semaphore));
    memory_->write_u32(sem_addr + 4, 3);  // Count = 3
    
    u32 result = XKernel::instance().wait_for_single_object(sem_addr, 0);
    EXPECT_EQ(result, WAIT_OBJECT_0);
    
    // Count should be decremented
    s32 count = static_cast<s32>(memory_->read_u32(sem_addr + 4));
    EXPECT_EQ(count, 2);
}

//=============================================================================
// Semaphore Operations Tests
//=============================================================================

TEST_F(XKernelTest, ReleaseSemaphore) {
    GuestAddr sem_addr = 0x00200000;
    
    memory_->write_u8(sem_addr, static_cast<u8>(XObjectType::Semaphore));
    memory_->write_u32(sem_addr + 4, 2);   // Count = 2
    memory_->write_u32(sem_addr + 16, 10); // Limit = 10
    
    s32 prev = XKernel::instance().release_semaphore(sem_addr, 5);
    
    EXPECT_EQ(prev, 2);
    
    s32 new_count = static_cast<s32>(memory_->read_u32(sem_addr + 4));
    EXPECT_EQ(new_count, 7);  // 2 + 5
}

//=============================================================================
// Mutant Operations Tests  
//=============================================================================

TEST_F(XKernelTest, ReleaseMutant) {
    GuestAddr mutant_addr = 0x00200000;
    
    memory_->write_u8(mutant_addr, static_cast<u8>(XObjectType::Mutant));
    memory_->write_u32(mutant_addr + 4, 0);   // SignalState = 0 (owned)
    memory_->write_u32(mutant_addr + 16, 0x12345678); // Owner
    
    s32 prev = XKernel::instance().release_mutant(mutant_addr);
    
    EXPECT_EQ(prev, 0);
    
    // Owner should be cleared
    u32 owner = memory_->read_u32(mutant_addr + 16);
    EXPECT_EQ(owner, 0u);
}

//=============================================================================
// Execution Tests
//=============================================================================

TEST_F(XKernelTest, RunForNoCrash) {
    // Just verify it doesn't crash with no threads
    XKernel::instance().run_for(1000);
    XKernel::instance().run_for(10000);
    XKernel::instance().run_for(100000);
}

TEST_F(XKernelTest, ProcessTimers) {
    // Should not crash
    XKernel::instance().process_timers();
}

TEST_F(XKernelTest, ProcessDpcs) {
    // Should not crash
    XKernel::instance().process_dpcs();
}

TEST_F(XKernelTest, ProcessApcs) {
    // Should not crash
    XKernel::instance().process_apcs();
}

//=============================================================================
// Helper Function Tests
//=============================================================================

TEST_F(XKernelTest, KeInitializeEvent) {
    GuestAddr event_addr = 0x00200000;
    
    xkernel::ke_initialize_event(event_addr, XEventType::NotificationEvent, true);
    
    u8 type = memory_->read_u8(event_addr);
    EXPECT_EQ(type, 0u);  // NotificationEvent = 0
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 1u);  // Initially signaled
}

TEST_F(XKernelTest, KeSetEvent) {
    GuestAddr event_addr = 0x00200000;
    xkernel::ke_initialize_event(event_addr, XEventType::NotificationEvent, false);
    
    s32 prev = xkernel::ke_set_event(event_addr);
    
    EXPECT_EQ(prev, 0);  // Was not signaled
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 1u);
}

TEST_F(XKernelTest, KeResetEvent) {
    GuestAddr event_addr = 0x00200000;
    xkernel::ke_initialize_event(event_addr, XEventType::NotificationEvent, true);
    
    s32 prev = xkernel::ke_reset_event(event_addr);
    
    EXPECT_EQ(prev, 1);  // Was signaled
    
    u32 signal_state = memory_->read_u32(event_addr + 4);
    EXPECT_EQ(signal_state, 0u);
}

TEST_F(XKernelTest, KeInitializeSemaphore) {
    GuestAddr sem_addr = 0x00200000;
    
    xkernel::ke_initialize_semaphore(sem_addr, 5, 10);
    
    u8 type = memory_->read_u8(sem_addr);
    EXPECT_EQ(type, static_cast<u8>(XObjectType::Semaphore));
    
    s32 count = static_cast<s32>(memory_->read_u32(sem_addr + 4));
    EXPECT_EQ(count, 5);
    
    s32 limit = static_cast<s32>(memory_->read_u32(sem_addr + 16));
    EXPECT_EQ(limit, 10);
}

TEST_F(XKernelTest, KeReleaseSemaphore) {
    GuestAddr sem_addr = 0x00200000;
    xkernel::ke_initialize_semaphore(sem_addr, 2, 10);
    
    s32 prev = xkernel::ke_release_semaphore(sem_addr, 3);
    
    EXPECT_EQ(prev, 2);
    
    s32 count = static_cast<s32>(memory_->read_u32(sem_addr + 4));
    EXPECT_EQ(count, 5);  // 2 + 3
}

TEST_F(XKernelTest, KeInitializeMutant) {
    GuestAddr mutant_addr = 0x00200000;
    
    xkernel::ke_initialize_mutant(mutant_addr, false);
    
    u8 type = memory_->read_u8(mutant_addr);
    EXPECT_EQ(type, static_cast<u8>(XObjectType::Mutant));
    
    s32 signal_state = static_cast<s32>(memory_->read_u32(mutant_addr + 4));
    EXPECT_EQ(signal_state, 1);  // Not owned = signaled
}

TEST_F(XKernelTest, KeInitializeDpc) {
    GuestAddr dpc_addr = 0x00200000;
    
    xkernel::ke_initialize_dpc(dpc_addr, 0x82001000, 0xDEADBEEF);
    
    u32 routine = memory_->read_u32(dpc_addr + 8);
    EXPECT_EQ(routine, 0x82001000u);
    
    u32 context = memory_->read_u32(dpc_addr + 12);
    EXPECT_EQ(context, 0xDEADBEEFu);
}

TEST_F(XKernelTest, KeInitializeTimer) {
    GuestAddr timer_addr = 0x00200000;
    
    xkernel::ke_initialize_timer(timer_addr);
    
    u8 type = memory_->read_u8(timer_addr);
    EXPECT_EQ(type, static_cast<u8>(XObjectType::TimerNotification));
}

TEST_F(XKernelTest, KeGetCurrentProcessorNumber) {
    // Without a current thread, should return 0
    u32 proc = xkernel::ke_get_current_processor_number();
    EXPECT_EQ(proc, 0u);
}

//=============================================================================
// Event Cache Tests
//=============================================================================

TEST_F(XKernelTest, GetOrCreateEventCaches) {
    GuestAddr event_addr = 0x00200000;
    
    memory_->write_u8(event_addr, 0);  // NotificationEvent
    memory_->write_u32(event_addr + 4, 0);
    
    // First call creates
    auto event1 = XKernel::instance().get_or_create_event(event_addr);
    ASSERT_NE(event1, nullptr);
    
    // Second call should return same object
    auto event2 = XKernel::instance().get_or_create_event(event_addr);
    ASSERT_NE(event2, nullptr);
    
    // Note: They might not be the same shared_ptr but represent same object
    EXPECT_EQ(event1->guest_object(), event2->guest_object());
}

TEST_F(XKernelTest, GetOrCreateEventNullAddr) {
    auto event = XKernel::instance().get_or_create_event(0);
    EXPECT_EQ(event, nullptr);
}

} // namespace test
} // namespace x360mu
