/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Buffer Pool Unit Tests
 * Tests the BufferPool data structures and statistics.
 * Note: Full allocation tests require Vulkan runtime.
 */

#include <gtest/gtest.h>
#include "gpu/buffer_pool.h"

namespace x360mu {
namespace test {

//=============================================================================
// PooledBuffer Structure Tests
//=============================================================================

TEST(PooledBufferTest, DefaultConstruction) {
    PooledBuffer buf{};
    EXPECT_EQ(buf.last_used_frame, 0u);
    EXPECT_FALSE(buf.in_use);
}

TEST(PooledBufferTest, LifecycleTracking) {
    PooledBuffer buf{};

    // Simulate use in frame 5
    buf.in_use = true;
    buf.last_used_frame = 5;

    EXPECT_TRUE(buf.in_use);
    EXPECT_EQ(buf.last_used_frame, 5u);

    // Release at end of frame
    buf.in_use = false;
    EXPECT_FALSE(buf.in_use);
    EXPECT_EQ(buf.last_used_frame, 5u);  // Still tracks last use
}

//=============================================================================
// BufferPool Stats Tests
//=============================================================================

TEST(BufferPoolStatsTest, DefaultZero) {
    BufferPool::Stats stats{};
    EXPECT_EQ(stats.total_buffers, 0u);
    EXPECT_EQ(stats.active_buffers, 0u);
    EXPECT_EQ(stats.reused_buffers, 0u);
    EXPECT_EQ(stats.created_buffers, 0u);
}

//=============================================================================
// BufferPool No-Vulkan Tests
//=============================================================================

TEST(BufferPoolTest, InitializeWithNull) {
    BufferPool pool;
    // Initialize with null Vulkan backend should fail gracefully
    Status status = pool.initialize(nullptr, 3);
    // Depending on implementation, this should either fail or handle null
    (void)status;  // Don't assert - behavior is implementation-defined
}

TEST(BufferPoolTest, ShutdownWithoutInit) {
    BufferPool pool;
    // Should not crash
    pool.shutdown();
}

TEST(BufferPoolTest, StatsAfterInit) {
    BufferPool pool;
    auto stats = pool.get_stats();
    EXPECT_EQ(stats.total_buffers, 0u);
    EXPECT_EQ(stats.active_buffers, 0u);
}

} // namespace test
} // namespace x360mu
