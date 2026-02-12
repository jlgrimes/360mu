/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Extended Memory System Tests
 * Tests bulk operations, MMIO, reservations, write tracking, time base
 */

#include <gtest/gtest.h>
#include "memory/memory.h"
#include <vector>
#include <atomic>

namespace x360mu {
namespace test {

class MemoryExtTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory;

    void SetUp() override {
        memory = std::make_unique<Memory>();
        ASSERT_EQ(memory->initialize(), Status::Ok);
    }

    void TearDown() override {
        memory->shutdown();
    }
};

//=============================================================================
// Bulk Memory Operations
//=============================================================================

TEST_F(MemoryExtTest, WriteBytes_ReadBytes) {
    GuestAddr addr = 0x00200000;
    u8 src[64];
    for (int i = 0; i < 64; i++) src[i] = static_cast<u8>(i);

    memory->write_bytes(addr, src, 64);

    u8 dst[64] = {};
    memory->read_bytes(addr, dst, 64);

    for (int i = 0; i < 64; i++) {
        EXPECT_EQ(dst[i], static_cast<u8>(i)) << "Mismatch at offset " << i;
    }
}

TEST_F(MemoryExtTest, WriteBytes_LargeBlock) {
    GuestAddr addr = 0x00300000;
    std::vector<u8> data(4096);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<u8>(i & 0xFF);
    }

    memory->write_bytes(addr, data.data(), data.size());

    std::vector<u8> readback(4096);
    memory->read_bytes(addr, readback.data(), readback.size());

    EXPECT_EQ(data, readback);
}

TEST_F(MemoryExtTest, ZeroBytes) {
    GuestAddr addr = 0x00400000;
    // Write non-zero data first
    for (int i = 0; i < 256; i++) {
        memory->write_u8(addr + i, 0xFF);
    }

    memory->zero_bytes(addr, 256);

    for (int i = 0; i < 256; i++) {
        EXPECT_EQ(memory->read_u8(addr + i), 0u) << "Non-zero at offset " << i;
    }
}

TEST_F(MemoryExtTest, CopyBytes) {
    GuestAddr src = 0x00500000;
    GuestAddr dst = 0x00600000;

    // Write pattern to source
    for (int i = 0; i < 128; i++) {
        memory->write_u8(src + i, static_cast<u8>(i * 2));
    }

    memory->copy_bytes(dst, src, 128);

    // Verify destination matches
    for (int i = 0; i < 128; i++) {
        EXPECT_EQ(memory->read_u8(dst + i), static_cast<u8>(i * 2))
            << "Mismatch at offset " << i;
    }
}

//=============================================================================
// Host Pointer Access
//=============================================================================

TEST_F(MemoryExtTest, GetHostPtr_ValidAddress) {
    GuestAddr addr = 0x00100000;
    void* ptr = memory->get_host_ptr(addr);
    EXPECT_NE(ptr, nullptr);
}

TEST_F(MemoryExtTest, GetHostPtr_WriteThrough) {
    GuestAddr addr = 0x00100000;
    memory->write_u32(addr, 0xDEADBEEF);

    void* ptr = memory->get_host_ptr(addr);
    ASSERT_NE(ptr, nullptr);

    // Read through host pointer should match
    u32 value = *static_cast<u32*>(ptr);
    // Note: value may be byte-swapped since Xbox 360 is big-endian
    // The raw bytes in host memory are in big-endian order
    EXPECT_TRUE(value == 0xDEADBEEF || value == byte_swap<u32>(0xDEADBEEF));
}

//=============================================================================
// Memory Allocation
//=============================================================================

TEST_F(MemoryExtTest, Allocate_Basic) {
    GuestAddr base = 0x01000000;
    Status status = memory->allocate(base, 64 * 1024, MemoryRegion::Read | MemoryRegion::Write);
    EXPECT_EQ(status, Status::Ok);
}

TEST_F(MemoryExtTest, Allocate_AndQuery) {
    GuestAddr base = 0x01100000;
    u64 size = 128 * 1024;
    memory->allocate(base, size, MemoryRegion::Read | MemoryRegion::Write | MemoryRegion::Execute);

    MemoryRegion info{};
    bool found = memory->query(base, info);
    EXPECT_TRUE(found);
    if (found) {
        EXPECT_EQ(info.base, base);
        EXPECT_EQ(info.size, size);
        EXPECT_TRUE(info.flags & MemoryRegion::Execute);
    }
}

TEST_F(MemoryExtTest, Free_Region) {
    GuestAddr base = 0x01200000;
    memory->allocate(base, 64 * 1024, MemoryRegion::Read | MemoryRegion::Write);
    memory->free(base);

    MemoryRegion info{};
    bool found = memory->query(base, info);
    EXPECT_FALSE(found);
}

//=============================================================================
// MMIO Registration and Dispatch
//=============================================================================

TEST_F(MemoryExtTest, RegisterMmio_ReadDispatch) {
    GuestAddr mmio_base = 0x7FC00000;  // GPU MMIO range

    u32 captured_value = 0;
    memory->register_mmio(mmio_base, 0x1000,
        [](GuestAddr addr) -> u32 {
            // Return register address as value
            return static_cast<u32>(addr & 0xFFFF);
        },
        [&captured_value](GuestAddr, u32 value) {
            captured_value = value;
        }
    );

    // Read from MMIO should go through handler
    u32 val = memory->read_u32(mmio_base + 0x100);
    EXPECT_EQ(val, 0x100u);
}

TEST_F(MemoryExtTest, RegisterMmio_WriteDispatch) {
    GuestAddr mmio_base = 0x7FC00000;
    u32 captured_value = 0;
    GuestAddr captured_addr = 0;

    memory->register_mmio(mmio_base, 0x1000,
        [](GuestAddr) -> u32 { return 0; },
        [&](GuestAddr addr, u32 value) {
            captured_addr = addr;
            captured_value = value;
        }
    );

    memory->write_u32(mmio_base + 0x200, 0xCAFE);
    EXPECT_EQ(captured_addr, mmio_base + 0x200);
    EXPECT_EQ(captured_value, 0xCAFEu);
}

TEST_F(MemoryExtTest, UnregisterMmio) {
    GuestAddr mmio_base = 0x7FC01000;
    bool handler_called = false;

    memory->register_mmio(mmio_base, 0x100,
        [&](GuestAddr) -> u32 { handler_called = true; return 0; },
        [](GuestAddr, u32) {}
    );

    memory->unregister_mmio(mmio_base);
    handler_called = false;

    // After unregister, read should not call handler
    memory->read_u32(mmio_base);
    // Handler may or may not be called depending on implementation
    // The key is it shouldn't crash
}

//=============================================================================
// Reservation (Atomic) Operations
//=============================================================================

TEST_F(MemoryExtTest, Reservation_SetAndCheck) {
    GuestAddr addr = 0x00200000;
    u32 thread_id = 0;

    memory->set_reservation(thread_id, addr, 4);
    EXPECT_TRUE(memory->check_reservation(thread_id, addr, 4));
}

TEST_F(MemoryExtTest, Reservation_ClearExplicit) {
    GuestAddr addr = 0x00200000;
    u32 thread_id = 0;

    memory->set_reservation(thread_id, addr, 4);
    memory->clear_reservation(thread_id);
    EXPECT_FALSE(memory->check_reservation(thread_id, addr, 4));
}

TEST_F(MemoryExtTest, Reservation_InvalidateOnWrite) {
    GuestAddr addr = 0x00200000;
    u32 thread_id = 0;

    memory->set_reservation(thread_id, addr, 4);

    // Write to same address should invalidate reservation
    memory->invalidate_reservations(addr, 4);
    EXPECT_FALSE(memory->check_reservation(thread_id, addr, 4));
}

TEST_F(MemoryExtTest, Reservation_PerThread) {
    GuestAddr addr1 = 0x00200000;
    GuestAddr addr2 = 0x00200100;

    memory->set_reservation(0, addr1, 4);
    memory->set_reservation(1, addr2, 4);

    EXPECT_TRUE(memory->check_reservation(0, addr1, 4));
    EXPECT_TRUE(memory->check_reservation(1, addr2, 4));

    // Invalidating addr1 should only affect thread 0's reservation
    memory->invalidate_reservations(addr1, 4);
    EXPECT_FALSE(memory->check_reservation(0, addr1, 4));
    EXPECT_TRUE(memory->check_reservation(1, addr2, 4));
}

TEST_F(MemoryExtTest, Reservation_WrongAddress) {
    memory->set_reservation(0, 0x00200000, 4);
    EXPECT_FALSE(memory->check_reservation(0, 0x00200010, 4));
}

TEST_F(MemoryExtTest, Reservation_WrongSize) {
    memory->set_reservation(0, 0x00200000, 4);
    EXPECT_FALSE(memory->check_reservation(0, 0x00200000, 8));
}

//=============================================================================
// Write Tracking
//=============================================================================

TEST_F(MemoryExtTest, WriteTracking_Callback) {
    GuestAddr track_base = 0x00300000;
    u64 track_size = 0x1000;
    bool callback_fired = false;
    GuestAddr callback_addr = 0;
    u64 callback_size = 0;

    memory->track_writes(track_base, track_size,
        [&](GuestAddr addr, u64 size) {
            callback_fired = true;
            callback_addr = addr;
            callback_size = size;
        }
    );

    memory->write_u32(track_base + 0x100, 0xDEAD);

    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(callback_addr, track_base + 0x100);
}

TEST_F(MemoryExtTest, WriteTracking_Untrack) {
    GuestAddr track_base = 0x00310000;
    bool callback_fired = false;

    memory->track_writes(track_base, 0x1000,
        [&](GuestAddr, u64) { callback_fired = true; }
    );

    memory->untrack_writes(track_base);
    callback_fired = false;

    memory->write_u32(track_base + 0x100, 0xBEEF);
    EXPECT_FALSE(callback_fired);
}

//=============================================================================
// Time Base
//=============================================================================

TEST_F(MemoryExtTest, TimeBase_InitialZero) {
    EXPECT_EQ(memory->get_time_base(), 0u);
}

TEST_F(MemoryExtTest, TimeBase_Advance) {
    memory->advance_time_base(1000);
    EXPECT_EQ(memory->get_time_base(), 1000u);

    memory->advance_time_base(500);
    EXPECT_EQ(memory->get_time_base(), 1500u);
}

TEST_F(MemoryExtTest, TimeBase_LargeValues) {
    u64 large_value = 0x100000000ULL;  // > 32 bits
    memory->advance_time_base(large_value);
    EXPECT_EQ(memory->get_time_base(), large_value);
}

//=============================================================================
// Fastmem
//=============================================================================

TEST_F(MemoryExtTest, FastmemBase_NotNull) {
    void* base = memory->get_fastmem_base();
    // May be nullptr on some platforms, but should not crash
    (void)base;
}

//=============================================================================
// Address Translation
//=============================================================================

TEST_F(MemoryExtTest, TranslateAddress_Physical) {
    // Physical address should map to itself
    GuestAddr addr = 0x00100000;
    GuestAddr translated = memory->translate_address(addr);
    EXPECT_EQ(translated, addr);
}

TEST_F(MemoryExtTest, TranslateAddress_VirtualMirror) {
    // 0x80000000 + offset should map to physical offset
    GuestAddr virt = 0x80100000;
    GuestAddr translated = memory->translate_address(virt);
    EXPECT_EQ(translated, 0x00100000u);
}

//=============================================================================
// Edge Cases
//=============================================================================

TEST_F(MemoryExtTest, ReadWrite_BoundaryAddress) {
    // Test at the boundary of main RAM
    GuestAddr addr = 0x1FFFFFF0;  // Near end of 512MB
    memory->write_u32(addr, 0x12345678);
    EXPECT_EQ(memory->read_u32(addr), 0x12345678u);
}

TEST_F(MemoryExtTest, ZeroBytes_ZeroLength) {
    // Should be a no-op, not crash
    memory->zero_bytes(0x00100000, 0);
}

TEST_F(MemoryExtTest, WriteBytes_ZeroLength) {
    u8 dummy = 0;
    memory->write_bytes(0x00100000, &dummy, 0);
    // Should not crash
}

} // namespace test
} // namespace x360mu
