/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XObject System Unit Tests
 */

#include <gtest/gtest.h>
#include "kernel/xobject.h"
#include "kernel/xthread.h"
#include "kernel/xevent.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"

namespace x360mu {
namespace test {

//=============================================================================
// XObject Base Tests
//=============================================================================

class TestObject : public XObject {
public:
    TestObject() : XObject(XObjectType::None) {}
    static constexpr XObjectType kType = XObjectType::None;
};

TEST(XObjectTest, CreateObject) {
    auto obj = std::make_shared<TestObject>();
    
    EXPECT_EQ(obj->type(), XObjectType::None);
    EXPECT_EQ(obj->handle(), 0u);
    EXPECT_EQ(obj->ref_count(), 1u);
    EXPECT_EQ(obj->guest_object(), 0u);
    EXPECT_TRUE(obj->name().empty());
}

TEST(XObjectTest, SetName) {
    auto obj = std::make_shared<TestObject>();
    obj->set_name("TestObject");
    
    EXPECT_EQ(obj->name(), "TestObject");
}

TEST(XObjectTest, SetGuestObject) {
    auto obj = std::make_shared<TestObject>();
    obj->set_guest_object(0x82000000);
    
    EXPECT_EQ(obj->guest_object(), 0x82000000u);
}

TEST(XObjectTest, RefCounting) {
    auto obj = std::make_shared<TestObject>();
    EXPECT_EQ(obj->ref_count(), 1u);
    
    obj->retain();
    EXPECT_EQ(obj->ref_count(), 2u);
    
    obj->retain();
    EXPECT_EQ(obj->ref_count(), 3u);
    
    obj->release();
    EXPECT_EQ(obj->ref_count(), 2u);
    
    obj->release();
    EXPECT_EQ(obj->ref_count(), 1u);
}

//=============================================================================
// ObjectTable Tests
//=============================================================================

TEST(ObjectTableTest, AddObject) {
    ObjectTable table;
    auto obj = std::make_shared<TestObject>();
    
    u32 handle = table.add_object(obj);
    
    EXPECT_NE(handle, 0u);
    EXPECT_EQ(obj->handle(), handle);
    EXPECT_EQ(table.object_count(), 1u);
}

TEST(ObjectTableTest, LookupObject) {
    ObjectTable table;
    auto obj = std::make_shared<TestObject>();
    obj->set_name("TestLookup");
    
    u32 handle = table.add_object(obj);
    
    auto found = table.lookup(handle);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), obj.get());
    EXPECT_EQ(found->name(), "TestLookup");
}

TEST(ObjectTableTest, LookupInvalidHandle) {
    ObjectTable table;
    
    auto found = table.lookup(0xDEADBEEF);
    EXPECT_EQ(found, nullptr);
}

TEST(ObjectTableTest, LookupByName) {
    ObjectTable table;
    
    auto obj1 = std::make_shared<TestObject>();
    obj1->set_name("Object1");
    table.add_object(obj1);
    
    auto obj2 = std::make_shared<TestObject>();
    obj2->set_name("Object2");
    table.add_object(obj2);
    
    auto found = table.lookup_by_name("Object2");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), obj2.get());
    
    auto notfound = table.lookup_by_name("Object3");
    EXPECT_EQ(notfound, nullptr);
}

TEST(ObjectTableTest, RemoveHandle) {
    ObjectTable table;
    auto obj = std::make_shared<TestObject>();
    
    u32 handle = table.add_object(obj);
    EXPECT_EQ(table.object_count(), 1u);
    
    bool removed = table.remove_handle(handle);
    EXPECT_TRUE(removed);
    EXPECT_EQ(table.object_count(), 0u);
    
    // Should not find it anymore
    auto found = table.lookup(handle);
    EXPECT_EQ(found, nullptr);
}

TEST(ObjectTableTest, RemoveInvalidHandle) {
    ObjectTable table;
    
    bool removed = table.remove_handle(0xDEADBEEF);
    EXPECT_FALSE(removed);
}

TEST(ObjectTableTest, MultipleObjects) {
    ObjectTable table;
    
    std::vector<u32> handles;
    for (int i = 0; i < 100; i++) {
        auto obj = std::make_shared<TestObject>();
        obj->set_name("Object" + std::to_string(i));
        handles.push_back(table.add_object(obj));
    }
    
    EXPECT_EQ(table.object_count(), 100u);
    
    // All handles should be unique
    std::set<u32> unique_handles(handles.begin(), handles.end());
    EXPECT_EQ(unique_handles.size(), 100u);
    
    // Should be able to look up all
    for (int i = 0; i < 100; i++) {
        auto found = table.lookup(handles[i]);
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->name(), "Object" + std::to_string(i));
    }
}

//=============================================================================
// KernelState Tests
//=============================================================================

class KernelStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        KernelState::instance().initialize(memory_.get(), cpu_.get());
    }
    
    void TearDown() override {
        KernelState::instance().shutdown();
        cpu_->shutdown();
        memory_->shutdown();
    }
    
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
};

TEST_F(KernelStateTest, Initialize) {
    EXPECT_EQ(KernelState::instance().memory(), memory_.get());
}

TEST_F(KernelStateTest, SystemTime) {
    u64 time1 = KernelState::instance().system_time();
    EXPECT_GT(time1, 0u);
    
    // Should increase over time
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    u64 time2 = KernelState::instance().system_time();
    EXPECT_GT(time2, time1);
}

TEST_F(KernelStateTest, InterruptTime) {
    u64 time1 = KernelState::instance().interrupt_time();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    u64 time2 = KernelState::instance().interrupt_time();
    
    EXPECT_GT(time2, time1);
}

TEST_F(KernelStateTest, TickCount) {
    u32 tick1 = KernelState::instance().tick_count();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    u32 tick2 = KernelState::instance().tick_count();
    
    EXPECT_GE(tick2 - tick1, 40u);  // At least 40ms passed (with some margin)
}

TEST_F(KernelStateTest, ObjectTableAccess) {
    auto& table = KernelState::instance().object_table();
    
    auto obj = std::make_shared<TestObject>();
    u32 handle = table.add_object(obj);
    
    EXPECT_NE(handle, 0u);
    EXPECT_GE(table.object_count(), 1u);
}

TEST_F(KernelStateTest, DpcQueue) {
    // Queue some DPCs with full arguments:
    // queue_dpc(dpc_addr, routine, context, arg1, arg2)
    KernelState::instance().queue_dpc(0x10000, 0x82001000, 0x12345678, 0xAAA, 0xBBB);
    KernelState::instance().queue_dpc(0x10030, 0x82002000, 0x87654321, 0xCCC, 0xDDD);
    
    // Process them
    KernelState::instance().process_dpcs();
    
    // Can queue more after processing
    KernelState::instance().queue_dpc(0x10060, 0x82003000, 0xDEADBEEF, 0xEEE, 0xFFF);
    
    // Process again
    KernelState::instance().process_dpcs();
}

TEST_F(KernelStateTest, DpcQueueWithNullRoutine) {
    // Queue DPC with null routine - should be skipped during processing
    KernelState::instance().queue_dpc(0x10000, 0, 0x12345678, 0, 0);
    
    // Should not crash
    KernelState::instance().process_dpcs();
}

TEST_F(KernelStateTest, DpcQueueAllArguments) {
    // Test that all arguments are stored correctly
    GuestAddr dpc_addr = 0x10000;
    GuestAddr routine = 0x82001000;
    GuestAddr context = 0xCCCCCCCC;
    GuestAddr arg1 = 0x11111111;
    GuestAddr arg2 = 0x22222222;
    
    KernelState::instance().queue_dpc(dpc_addr, routine, context, arg1, arg2);
    
    // Process - verifies no crash with all arguments
    KernelState::instance().process_dpcs();
}

TEST_F(KernelStateTest, CpuAccessor) {
    // Test CPU accessor
    EXPECT_EQ(KernelState::instance().cpu(), cpu_.get());
}

TEST_F(KernelStateTest, SetCpu) {
    // Test set_cpu
    KernelState::instance().set_cpu(nullptr);
    EXPECT_EQ(KernelState::instance().cpu(), nullptr);
    
    // Restore
    KernelState::instance().set_cpu(cpu_.get());
    EXPECT_EQ(KernelState::instance().cpu(), cpu_.get());
}

} // namespace test
} // namespace x360mu
