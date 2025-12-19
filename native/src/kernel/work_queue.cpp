/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Work Queue Implementation
 */

#include "work_queue.h"
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-workq"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[WorkQ] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[WorkQ WARN] " __VA_ARGS__); printf("\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// WorkQueue Implementation
//=============================================================================

void WorkQueue::enqueue(const WorkQueueItem& item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(item);
        LOGI("Enqueued work item: routine=0x%08X, param=0x%08X, queue_size=%zu",
             (u32)item.worker_routine, (u32)item.parameter, items_.size());
    }
    cv_.notify_one();
}

bool WorkQueue::dequeue(WorkQueueItem& item, u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (timeout_ms == WORK_QUEUE_INFINITE_TIMEOUT) {
        // Infinite wait
        cv_.wait(lock, [this] { return !items_.empty() || shutdown_.load(); });
    } else if (timeout_ms > 0) {
        // Timed wait
        auto result = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [this] { return !items_.empty() || shutdown_.load(); });
        if (!result) {
            return false;  // Timeout
        }
    } else {
        // No wait - just check
        if (items_.empty()) {
            return false;
        }
    }
    
    if (shutdown_.load() && items_.empty()) {
        return false;
    }
    
    if (items_.empty()) {
        return false;
    }
    
    item = items_.front();
    items_.pop_front();
    
    LOGI("Dequeued work item: routine=0x%08X, param=0x%08X, remaining=%zu",
         (u32)item.worker_routine, (u32)item.parameter, items_.size());
    
    return true;
}

void WorkQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

bool WorkQueue::is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
}

size_t WorkQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

void WorkQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = false;
    items_.clear();
}

//=============================================================================
// WorkQueueManager Implementation
//=============================================================================

WorkQueueManager& WorkQueueManager::instance() {
    static WorkQueueManager instance;
    return instance;
}

void WorkQueueManager::enqueue(WorkQueueType type, const WorkQueueItem& item) {
    size_t queue_idx = static_cast<size_t>(type);
    if (queue_idx >= static_cast<size_t>(WorkQueueType::Maximum)) {
        queue_idx = static_cast<size_t>(WorkQueueType::Delayed);  // Default
    }
    
    queues_[queue_idx].enqueue(item);
    total_queued_++;
    
    LOGI("WorkQueueManager: enqueued to queue %zu, total_queued=%zu", 
         queue_idx, total_queued_.load());
}

bool WorkQueueManager::dequeue(WorkQueueType type, WorkQueueItem& item, u32 timeout_ms) {
    size_t queue_idx = static_cast<size_t>(type);
    if (queue_idx >= static_cast<size_t>(WorkQueueType::Maximum)) {
        queue_idx = static_cast<size_t>(WorkQueueType::Delayed);
    }
    
    bool result = queues_[queue_idx].dequeue(item, timeout_ms);
    if (result) {
        total_processed_++;
    }
    return result;
}

void WorkQueueManager::shutdown_all() {
    LOGI("WorkQueueManager: shutting down all queues");
    for (auto& queue : queues_) {
        queue.shutdown();
    }
}

void WorkQueueManager::reset_all() {
    LOGI("WorkQueueManager: resetting all queues");
    for (auto& queue : queues_) {
        queue.reset();
    }
    total_queued_ = 0;
    total_processed_ = 0;
}

size_t WorkQueueManager::get_queue_size(WorkQueueType type) const {
    size_t queue_idx = static_cast<size_t>(type);
    if (queue_idx >= static_cast<size_t>(WorkQueueType::Maximum)) {
        return 0;
    }
    return queues_[queue_idx].size();
}

size_t WorkQueueManager::get_total_queued() const {
    return total_queued_.load();
}

size_t WorkQueueManager::get_total_processed() const {
    return total_processed_.load();
}

} // namespace x360mu
