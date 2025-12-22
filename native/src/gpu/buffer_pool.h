/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Buffer Pool - Manages reusable Vulkan buffers for vertex/index data
 *
 * Implements frame-based lifecycle management to avoid creating
 * and destroying buffers every frame (which causes memory leaks and
 * performance issues).
 */

#pragma once

#include "x360mu/types.h"
#include "vulkan/vulkan_backend.h"
#include <vector>
#include <mutex>

namespace x360mu {

class VulkanBackend;

/**
 * Pooled buffer entry with lifecycle tracking
 */
struct PooledBuffer {
    VulkanBuffer buffer;           // Vulkan buffer with memory
    u32 last_used_frame;           // Last frame this buffer was used
    bool in_use;                   // Currently in use this frame

    PooledBuffer() : last_used_frame(0), in_use(false) {}
};

/**
 * Buffer Pool
 *
 * Manages a pool of Vulkan buffers that can be reused across frames.
 * Buffers are allocated on demand and reused after N frames to avoid
 * destroying buffers that may still be in use by the GPU.
 */
class BufferPool {
public:
    /**
     * Initialize buffer pool
     * @param vulkan Vulkan backend for buffer creation
     * @param frames_until_reuse Number of frames before a buffer can be reused (default: 3)
     */
    Status initialize(VulkanBackend* vulkan, u32 frames_until_reuse = 3);

    /**
     * Shutdown and cleanup all buffers
     */
    void shutdown();

    /**
     * Allocate a buffer from the pool
     * @param size Size in bytes
     * @param current_frame Current frame index
     * @return Vulkan buffer, or VK_NULL_HANDLE on failure
     */
    VkBuffer allocate(size_t size, u32 current_frame);

    /**
     * Get the host-visible mapped pointer for a buffer
     * @param buffer Vulkan buffer handle
     * @return Mapped pointer, or nullptr if not found/not mapped
     */
    void* get_mapped_ptr(VkBuffer buffer);

    /**
     * Mark frame complete - releases buffers from previous frames
     * @param current_frame Current frame index
     */
    void end_frame(u32 current_frame);

    /**
     * Get statistics
     */
    struct Stats {
        u32 total_buffers;
        u32 active_buffers;
        u32 reused_buffers;
        u32 created_buffers;
    };
    Stats get_stats() const { return stats_; }

private:
    VulkanBackend* vulkan_ = nullptr;
    u32 frames_until_reuse_ = 3;

    // Pool of buffers
    std::vector<PooledBuffer> buffers_;

    // Thread safety
    std::mutex mutex_;

    // Statistics
    Stats stats_{};

    // Helper: Find a free buffer of at least the given size
    PooledBuffer* find_free_buffer(size_t size, u32 current_frame);

    // Helper: Create a new buffer
    PooledBuffer* create_buffer(size_t size);

    // Helper: Cleanup old buffers that haven't been used recently
    void cleanup_old_buffers(u32 current_frame);
};

} // namespace x360mu
