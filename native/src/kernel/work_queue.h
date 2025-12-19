/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Work Queue System
 * 
 * Implements Windows NT-style work queues for Xbox 360 kernel emulation.
 * Games use ExQueueWorkItem to queue work to system worker threads.
 */

#pragma once

#include "x360mu/types.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <limits>

namespace x360mu {

// Mirrors Xbox 360 WORK_QUEUE_ITEM structure layout
// struct _WORK_QUEUE_ITEM {
//     LIST_ENTRY List;              // Offset 0x00 (Flink, Blink)
//     PWORKER_THREAD_ROUTINE WorkerRoutine;  // Offset 0x08
//     PVOID Parameter;              // Offset 0x0C
// };
struct WorkQueueItem {
    GuestAddr list_flink;      // Offset 0x00
    GuestAddr list_blink;      // Offset 0x04
    GuestAddr worker_routine;  // Offset 0x08 - Guest function pointer
    GuestAddr parameter;       // Offset 0x0C - Context parameter
    
    // Host-side tracking
    GuestAddr item_address;    // Address of this item in guest memory
};

// WORK_QUEUE_TYPE enumeration
enum class WorkQueueType : u32 {
    Critical = 0,       // CriticalWorkQueue - High priority
    Delayed = 1,        // DelayedWorkQueue - Normal priority
    HyperCritical = 2,  // HyperCriticalWorkQueue
    Maximum = 3
};

// Thread-safe work queue
class WorkQueue {
public:
    WorkQueue() = default;
    ~WorkQueue() = default;
    
    // Enqueue a work item (non-blocking)
    void enqueue(const WorkQueueItem& item);
    
    // Dequeue a work item (blocking with timeout)
    // Returns true if item was dequeued, false on timeout or shutdown
    bool dequeue(WorkQueueItem& item, u32 timeout_ms);
    
    // Signal shutdown to unblock waiting threads
    void shutdown();
    
    // Check if queue is empty
    bool is_empty() const;
    
    // Get current queue size
    size_t size() const;
    
    // Reset shutdown flag (for restart)
    void reset();
    
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WorkQueueItem> items_;
    std::atomic<bool> shutdown_{false};
};

// Global work queue manager (singleton)
class WorkQueueManager {
public:
    static WorkQueueManager& instance();
    
    // Enqueue work item to specified queue type
    void enqueue(WorkQueueType type, const WorkQueueItem& item);
    
    // Dequeue work item from specified queue type
    // Returns true if item was dequeued
    bool dequeue(WorkQueueType type, WorkQueueItem& item, u32 timeout_ms);
    
    // Shutdown all queues
    void shutdown_all();
    
    // Reset all queues
    void reset_all();
    
    // Get queue statistics
    size_t get_queue_size(WorkQueueType type) const;
    size_t get_total_queued() const;
    size_t get_total_processed() const;
    
private:
    WorkQueueManager() = default;
    ~WorkQueueManager() = default;
    
    WorkQueue queues_[static_cast<size_t>(WorkQueueType::Maximum)];
    std::atomic<size_t> total_queued_{0};
    std::atomic<size_t> total_processed_{0};
};

// Constants
constexpr u32 WORK_QUEUE_INFINITE_TIMEOUT = std::numeric_limits<u32>::max();

} // namespace x360mu
