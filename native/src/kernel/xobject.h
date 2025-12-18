/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Object System
 * Based on Xbox 360 kernel object model (similar to Windows NT)
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <list>

namespace x360mu {

class Memory;
class XThread;
class XObject;

/**
 * Kernel object types (from Xbox 360 kernel)
 */
enum class XObjectType : u8 {
    None = 0,
    
    // Dispatcher objects (can be waited on)
    NotificationEvent = 0,    // Manual reset event
    SynchronizationEvent = 1, // Auto reset event
    Mutant = 2,               // Mutex
    Process = 3,
    Queue = 4,
    Semaphore = 5,
    Thread = 6,
    Gate = 7,                 // Undocumented
    TimerNotification = 8,
    TimerSynchronization = 9,
    
    // Other kernel objects
    File = 16,
    IoCompletion = 17,
    Module = 18,
    Symbolic = 19,
    
    // Custom tracking
    MaxType = 32
};

/**
 * Object attributes flags
 */
enum ObjectAttributes : u32 {
    OBJ_INHERIT = 0x00000002,
    OBJ_PERMANENT = 0x00000010,
    OBJ_EXCLUSIVE = 0x00000020,
    OBJ_CASE_INSENSITIVE = 0x00000040,
    OBJ_OPENIF = 0x00000080,
    OBJ_OPENLINK = 0x00000100,
    OBJ_KERNEL_HANDLE = 0x00000200,
};

/**
 * Wait result codes
 */
enum WaitResult : u32 {
    WAIT_OBJECT_0 = 0x00000000,
    WAIT_ABANDONED = 0x00000080,
    WAIT_IO_COMPLETION = 0x000000C0,  // STATUS_USER_APC - wait completed due to APC
    WAIT_ALERTED = 0x00000101,         // STATUS_ALERTED - thread was alerted
    WAIT_TIMEOUT = 0x00000102,
    WAIT_FAILED = 0xFFFFFFFF,
};

// Note: DispatcherHeader is defined in threading.h
// We use that definition for compatibility

/**
 * Base class for all kernel objects
 */
class XObject : public std::enable_shared_from_this<XObject> {
public:
    XObject(XObjectType type);
    virtual ~XObject();
    
    // Object identification
    XObjectType type() const { return type_; }
    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }
    
    // Handle management
    u32 handle() const { return handle_; }
    void set_handle(u32 handle) { handle_ = handle; }
    
    // Reference counting
    void retain();
    void release();
    u32 ref_count() const { return ref_count_; }
    
    // Guest memory representation (if any)
    GuestAddr guest_object() const { return guest_object_; }
    void set_guest_object(GuestAddr addr) { guest_object_ = addr; }
    
    // Wait support (for dispatcher objects)
    virtual bool is_signaled() const { return false; }
    virtual void signal() {}
    virtual void unsignal() {}
    
    // Wait list management
    void add_waiter(XThread* thread);
    void remove_waiter(XThread* thread);
    void wake_waiters(u32 count = UINT32_MAX);
    
protected:
    XObjectType type_;
    std::string name_;
    u32 handle_ = 0;
    std::atomic<u32> ref_count_{1};
    GuestAddr guest_object_ = 0;
    
    // Waiters list
    std::mutex waiters_mutex_;
    std::list<XThread*> waiters_;
};

/**
 * Object handle table
 */
class ObjectTable {
public:
    ObjectTable();
    ~ObjectTable();
    
    // Handle operations
    u32 add_object(std::shared_ptr<XObject> object);
    bool remove_handle(u32 handle);
    std::shared_ptr<XObject> lookup(u32 handle);
    
    template<typename T>
    std::shared_ptr<T> lookup_typed(u32 handle) {
        auto obj = lookup(handle);
        if (obj && obj->type() == T::kType) {
            return std::static_pointer_cast<T>(obj);
        }
        return nullptr;
    }
    
    // Find by name
    std::shared_ptr<XObject> lookup_by_name(const std::string& name);
    
    // Statistics
    size_t object_count() const;
    
    // Clear all objects (for shutdown)
    void clear();
    
private:
    std::mutex mutex_;
    std::unordered_map<u32, std::shared_ptr<XObject>> objects_;
    u32 next_handle_ = 0x10000;  // Start handles at a reasonable value
};

/**
 * Global kernel state
 */
class KernelState {
public:
    static KernelState& instance();
    
    void initialize(Memory* memory);
    void shutdown();
    
    Memory* memory() const { return memory_; }
    ObjectTable& object_table() { return object_table_; }
    
    // System-wide state
    u64 system_time() const;  // 100ns intervals since Jan 1, 1601
    u64 interrupt_time() const;  // Time since boot
    u32 tick_count() const;  // Milliseconds since boot
    
    // Process/thread tracking
    void set_current_thread(XThread* thread);
    XThread* current_thread() const;
    
    // DPC support
    void queue_dpc(GuestAddr dpc_routine, GuestAddr context);
    void process_dpcs();
    
private:
    KernelState() = default;
    
    Memory* memory_ = nullptr;
    ObjectTable object_table_;
    
    // Time tracking
    std::chrono::steady_clock::time_point boot_time_;
    
    // Per-thread current thread (using thread_local)
    static thread_local XThread* current_thread_;
    
    // DPC queue
    struct DpcEntry {
        GuestAddr routine;
        GuestAddr context;
    };
    std::mutex dpc_mutex_;
    std::vector<DpcEntry> dpc_queue_;
};

// Helper macros for object type checking
#define X_OBJECT_TYPE(cls, type_val) \
    static constexpr XObjectType kType = type_val; \
    static bool is_type(XObjectType t) { return t == kType; }

} // namespace x360mu
