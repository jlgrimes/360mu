/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Buffer Pool Implementation
 */

#include "buffer_pool.h"
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-bufferpool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[BUFFERPOOL] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[BUFFERPOOL WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[BUFFERPOOL ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

Status BufferPool::initialize(VulkanBackend* vulkan, u32 frames_until_reuse) {
    if (!vulkan) {
        LOGE("Cannot initialize buffer pool: null Vulkan backend");
        return Status::Error;
    }

    vulkan_ = vulkan;
    frames_until_reuse_ = frames_until_reuse;

    stats_ = Stats{};

    LOGI("Buffer pool initialized (frames_until_reuse=%u)", frames_until_reuse);
    return Status::Ok;
}

void BufferPool::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Destroy all buffers
    for (auto& pooled : buffers_) {
        if (pooled.buffer.buffer != VK_NULL_HANDLE) {
            vulkan_->destroy_buffer(pooled.buffer);
        }
    }

    buffers_.clear();
    stats_ = Stats{};

    LOGI("Buffer pool shutdown");
    vulkan_ = nullptr;
}

VkBuffer BufferPool::allocate(size_t size, u32 current_frame) {
    if (!vulkan_ || size == 0) {
        return VK_NULL_HANDLE;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Try to find a free buffer of suitable size
    PooledBuffer* pooled = find_free_buffer(size, current_frame);

    // If no suitable buffer found, create a new one
    if (!pooled) {
        pooled = create_buffer(size);
        if (!pooled) {
            LOGE("Failed to create buffer of size %zu", size);
            return VK_NULL_HANDLE;
        }
        stats_.created_buffers++;
    } else {
        stats_.reused_buffers++;
    }

    // Mark as in use
    pooled->in_use = true;
    pooled->last_used_frame = current_frame;

    stats_.active_buffers++;

    return pooled->buffer.buffer;
}

void* BufferPool::get_mapped_ptr(VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Find the buffer in our pool
    for (auto& pooled : buffers_) {
        if (pooled.buffer.buffer == buffer) {
            return pooled.buffer.mapped;
        }
    }

    return nullptr;
}

void BufferPool::end_frame(u32 current_frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Mark all buffers as not in use for this frame
    for (auto& pooled : buffers_) {
        pooled.in_use = false;
    }

    stats_.active_buffers = 0;

    // Cleanup old buffers periodically (every 60 frames)
    if (current_frame % 60 == 0) {
        cleanup_old_buffers(current_frame);
    }
}

PooledBuffer* BufferPool::find_free_buffer(size_t size, u32 current_frame) {
    // Look for a buffer that:
    // 1. Is not currently in use
    // 2. Hasn't been used for at least frames_until_reuse_ frames
    // 3. Is large enough for the requested size

    for (auto& pooled : buffers_) {
        // Check if buffer is free
        if (pooled.in_use) {
            continue;
        }

        // Check if enough frames have passed
        if (current_frame < pooled.last_used_frame + frames_until_reuse_) {
            continue;
        }

        // Check if buffer is large enough
        if (pooled.buffer.size < size) {
            continue;
        }

        // Found a suitable buffer
        return &pooled;
    }

    return nullptr;
}

PooledBuffer* BufferPool::create_buffer(size_t size) {
    // Create a new Vulkan buffer (host-visible for CPU writes)
    VulkanBuffer buffer = vulkan_->create_buffer(
        size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (buffer.buffer == VK_NULL_HANDLE) {
        return nullptr;
    }

    // Map the buffer for CPU access
    if (buffer.mapped == nullptr) {
        VkResult result = vkMapMemory(
            vulkan_->device(),
            buffer.memory,
            0,
            size,
            0,
            &buffer.mapped
        );

        if (result != VK_SUCCESS) {
            LOGE("Failed to map buffer memory");
            vulkan_->destroy_buffer(buffer);
            return nullptr;
        }
    }

    // Add to pool
    PooledBuffer pooled;
    pooled.buffer = buffer;
    pooled.last_used_frame = 0;
    pooled.in_use = false;

    buffers_.push_back(pooled);
    stats_.total_buffers++;

    LOGD("Created new buffer: size=%zu, total_buffers=%u",
         size, stats_.total_buffers);

    return &buffers_.back();
}

void BufferPool::cleanup_old_buffers(u32 current_frame) {
    // Remove buffers that haven't been used in a long time (120 frames = ~2 seconds at 60 FPS)
    const u32 cleanup_threshold = 120;

    auto it = buffers_.begin();
    while (it != buffers_.end()) {
        if (current_frame > it->last_used_frame + cleanup_threshold) {
            // Buffer hasn't been used recently, destroy it
            LOGD("Cleaning up old buffer (last used: frame %u, current: %u)",
                 it->last_used_frame, current_frame);

            vulkan_->destroy_buffer(it->buffer);
            it = buffers_.erase(it);
            stats_.total_buffers--;
        } else {
            ++it;
        }
    }
}

} // namespace x360mu
