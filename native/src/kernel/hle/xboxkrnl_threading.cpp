/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * xboxkrnl.exe Threading HLE Functions
 * 
 * This file implements the threading and synchronization syscalls from xboxkrnl.exe:
 * - Thread creation and management
 * - Events, Semaphores, Mutants
 * - Critical Sections
 * - Wait functions
 * - Thread Local Storage
 */

#include "../kernel.h"
#include "../threading.h"
#include "../work_queue.h"
#include "../xobject.h"
#include "../../cpu/xenon/cpu.h"
#include "../../cpu/xenon/threading.h"
#include "../../memory/memory.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-hle-thread"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[HLE-THREAD] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[HLE-THREAD WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[HLE-THREAD ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// Global kernel thread manager (set by kernel initialization)
static KernelThreadManager* g_ktm = nullptr;

void set_kernel_threading_hle(KernelThreadManager* ktm) {
    g_ktm = ktm;
}

//=============================================================================
// Thread Management HLE Functions
//=============================================================================

/**
 * ExCreateThread - Create a new thread
 * 
 * Ordinal: 14
 * 
 * NTSTATUS ExCreateThread(
 *   PHANDLE pHandle,          // arg[0] - OUT: thread handle
 *   SIZE_T StackSize,         // arg[1] - stack size in bytes
 *   PDWORD pThreadId,         // arg[2] - OUT: thread ID
 *   PVOID ApiThreadStartup,   // arg[3] - XAPI startup wrapper (or NULL)
 *   PVOID StartRoutine,       // arg[4] - thread entry point
 *   PVOID StartContext,       // arg[5] - parameter passed to thread
 *   DWORD CreationFlags       // arg[6] - CREATE_SUSPENDED, etc.
 * );
 */
static void HLE_ExCreateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 stack_size = static_cast<u32>(args[1]);
    GuestAddr thread_id_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr xapi_startup = static_cast<GuestAddr>(args[3]);
    GuestAddr start_routine = static_cast<GuestAddr>(args[4]);
    GuestAddr start_context = static_cast<GuestAddr>(args[5]);
    u32 creation_flags = static_cast<u32>(args[6]);
    
    LOGI("ExCreateThread: stack=0x%X, entry=0x%08X, param=0x%08X, flags=0x%X",
         stack_size, start_routine, start_context, creation_flags);
    
    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 handle = 0;
    u32 thread_id = 0;
    
    u32 status = g_ktm->create_thread(&handle, stack_size, &thread_id,
                                       xapi_startup, start_routine,
                                       start_context, creation_flags);
    
    if (status == nt::STATUS_SUCCESS) {
        if (handle_ptr) memory->write_u32(handle_ptr, handle);
        if (thread_id_ptr) memory->write_u32(thread_id_ptr, thread_id);
    }
    
    *result = status;
}

/**
 * NtTerminateThread - Terminate a thread
 * 
 * Ordinal: 216
 */
static void HLE_NtTerminateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    u32 exit_code = static_cast<u32>(args[1]);
    
    LOGD("NtTerminateThread: handle=0x%X, exit=0x%X", handle, exit_code);
    
    *result = g_ktm ? g_ktm->terminate_thread(handle, exit_code) : nt::STATUS_UNSUCCESSFUL;
}

/**
 * NtSuspendThread - Suspend a thread
 * 
 * Ordinal: 215
 */
static void HLE_NtSuspendThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[1]);
    
    u32 prev_count = 0;
    u32 status = g_ktm ? g_ktm->suspend_thread(handle, &prev_count) : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_count_ptr) {
        memory->write_u32(prev_count_ptr, prev_count);
    }
    
    *result = status;
}

/**
 * NtResumeThread - Resume a thread
 * 
 * Ordinal: 209
 */
static void HLE_NtResumeThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[1]);
    
    u32 prev_count = 0;
    u32 status = g_ktm ? g_ktm->resume_thread(handle, &prev_count) : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_count_ptr) {
        memory->write_u32(prev_count_ptr, prev_count);
    }
    
    *result = status;
}

/**
 * KeGetCurrentProcessorNumber - Get current processor (0-5)
 * 
 * Ordinal: 49
 */
static void HLE_KeGetCurrentProcessorNumber(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = g_ktm ? g_ktm->get_current_processor() : 0;
}

/**
 * KeGetCurrentThread - Get KTHREAD pointer for current thread
 * 
 * Ordinal: 51
 */
static void HLE_KeGetCurrentThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Use GetCurrentGuestThread() to get the actual thread handle
    GuestThread* guest = GetCurrentGuestThread();
    u32 handle = 0x80000001;  // Default fallback
    
    if (guest) {
        handle = guest->handle;
    } else if (g_ktm) {
        handle = g_ktm->get_current_thread_handle();
    }
    
    // Return a pseudo-KTHREAD pointer based on handle
    // KTHREAD is at 0x80070000 + (handle & 0xFFFF) * 0x200
    *result = 0x80070000 + (handle & 0xFFFF) * 0x200;
    
    static int log_count = 0;
    if (log_count++ < 5) {
        LOGI("KeGetCurrentThread (threading): guest=%p, handle=0x%X, result=0x%llX",
             guest, handle, *result);
    }
}

/**
 * NtYieldExecution - Yield to other threads
 * 
 * Ordinal: 221
 */
static void HLE_NtYieldExecution(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    if (g_ktm) g_ktm->yield();
    *result = nt::STATUS_SUCCESS;
}

/**
 * KeSetAffinityThread - Set thread CPU affinity
 * 
 * Ordinal: 84
 */
static void HLE_KeSetAffinityThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr thread_ptr = static_cast<GuestAddr>(args[0]);
    u32 affinity = static_cast<u32>(args[1]);
    
    // Return old affinity (default to all threads)
    *result = 0x3F;
}

//=============================================================================
// APC (Asynchronous Procedure Call) Functions
//=============================================================================

/**
 * KeInitializeApc - Initialize an APC object
 * 
 * Ordinal: 106
 * 
 * VOID KeInitializeApc(
 *   PKAPC Apc,                    // arg[0] - APC object to initialize
 *   PKTHREAD Thread,              // arg[1] - Target thread
 *   PKKERNEL_ROUTINE KernelRoutine, // arg[2] - Kernel-mode routine
 *   PKRUNDOWN_ROUTINE RundownRoutine, // arg[3] - Rundown routine
 *   PKNORMAL_ROUTINE NormalRoutine,   // arg[4] - Normal (user-mode) routine
 *   KPROCESSOR_MODE ApcMode,      // arg[5] - KernelMode=0, UserMode=1
 *   PVOID NormalContext           // arg[6] - Context for normal routine
 * );
 * 
 * KAPC structure layout (Xbox 360):
 * +0x00: Type (SHORT) = 0x12 (ApcObject)
 * +0x02: ApcMode (CCHAR)
 * +0x03: Inserted (UCHAR)
 * +0x04: Thread pointer
 * +0x08: ApcListEntry (LIST_ENTRY)
 * +0x10: KernelRoutine
 * +0x14: RundownRoutine
 * +0x18: NormalRoutine
 * +0x1C: NormalContext
 * +0x20: SystemArgument1
 * +0x24: SystemArgument2
 */
static void HLE_KeInitializeApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr thread_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr kernel_routine = static_cast<GuestAddr>(args[2]);
    GuestAddr rundown_routine = static_cast<GuestAddr>(args[3]);
    GuestAddr normal_routine = static_cast<GuestAddr>(args[4]);
    u32 apc_mode = static_cast<u32>(args[5]);
    GuestAddr normal_context = static_cast<GuestAddr>(args[6]);
    
    LOGD("KeInitializeApc: apc=0x%08X, thread=0x%08X, kernel=0x%08X, normal=0x%08X",
         apc_ptr, thread_ptr, kernel_routine, normal_routine);
    
    // Initialize APC structure
    memory->write_u16(apc_ptr + 0x00, 0x12);        // Type = ApcObject
    memory->write_u8(apc_ptr + 0x02, apc_mode);     // ApcMode
    memory->write_u8(apc_ptr + 0x03, 0);            // Inserted = false
    memory->write_u32(apc_ptr + 0x04, thread_ptr);  // Thread
    memory->write_u32(apc_ptr + 0x08, 0);           // ApcListEntry.Flink
    memory->write_u32(apc_ptr + 0x0C, 0);           // ApcListEntry.Blink
    memory->write_u32(apc_ptr + 0x10, kernel_routine);
    memory->write_u32(apc_ptr + 0x14, rundown_routine);
    memory->write_u32(apc_ptr + 0x18, normal_routine);
    memory->write_u32(apc_ptr + 0x1C, normal_context);
    memory->write_u32(apc_ptr + 0x20, 0);           // SystemArgument1
    memory->write_u32(apc_ptr + 0x24, 0);           // SystemArgument2
    
    // No return value (void function)
}

/**
 * KeInsertQueueApc - Insert an APC into a thread's APC queue
 * 
 * Ordinal: 108
 * 
 * BOOLEAN KeInsertQueueApc(
 *   PKAPC Apc,                    // arg[0] - APC to insert
 *   PVOID SystemArgument1,        // arg[1] - First system argument
 *   PVOID SystemArgument2,        // arg[2] - Second system argument
 *   KPRIORITY Increment           // arg[3] - Priority increment (unused)
 * );
 */
static void HLE_KeInsertQueueApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr system_arg1 = static_cast<GuestAddr>(args[1]);
    GuestAddr system_arg2 = static_cast<GuestAddr>(args[2]);
    // u32 increment = static_cast<u32>(args[3]); // Unused
    
    // Check if already inserted
    u8 inserted = memory->read_u8(apc_ptr + 0x03);
    if (inserted) {
        LOGW("KeInsertQueueApc: APC already inserted");
        *result = 0;  // FALSE
        return;
    }
    
    // Read APC structure
    GuestAddr thread_ptr = memory->read_u32(apc_ptr + 0x04);
    u8 apc_mode = memory->read_u8(apc_ptr + 0x02);
    GuestAddr normal_routine = memory->read_u32(apc_ptr + 0x18);
    GuestAddr normal_context = memory->read_u32(apc_ptr + 0x1C);
    
    // Store system arguments in APC structure
    memory->write_u32(apc_ptr + 0x20, system_arg1);
    memory->write_u32(apc_ptr + 0x24, system_arg2);
    
    LOGI("KeInsertQueueApc: apc=0x%08X, thread=0x%08X, routine=0x%08X, mode=%u",
         apc_ptr, thread_ptr, normal_routine, apc_mode);
    
    if (!g_ktm) {
        LOGE("KeInsertQueueApc: Thread manager not initialized");
        *result = 0;  // FALSE
        return;
    }
    
    // Get the thread scheduler from the thread manager
    // We need to find the thread by its KTHREAD pointer
    // For now, we'll use the thread handle from the KTHREAD structure
    // The thread_id is stored at offset 0x8C in KTHREAD
    u32 thread_id = memory->read_u32(thread_ptr + 0x8C);
    
    // Get the kernel's thread scheduler
    ThreadScheduler* scheduler = get_thread_scheduler();
    
    if (!scheduler) {
        LOGE("KeInsertQueueApc: Scheduler not available");
        *result = 0;
        return;
    }
    
    GuestThread* thread = scheduler->get_thread(thread_id);
    if (!thread) {
        // Try to find by handle
        u32 handle = memory->read_u32(thread_ptr + 0x04);  // Approximate
        thread = scheduler->get_thread_by_handle(handle);
    }
    
    if (!thread) {
        LOGW("KeInsertQueueApc: Target thread not found (id=%u)", thread_id);
        *result = 0;  // FALSE
        return;
    }
    
    // Queue the APC
    bool kernel_mode = (apc_mode == 0);
    scheduler->queue_apc(thread, normal_routine, normal_context, 
                         system_arg1, system_arg2, kernel_mode);
    
    // Mark as inserted
    memory->write_u8(apc_ptr + 0x03, 1);
    
    *result = 1;  // TRUE
}

/**
 * KeRemoveQueueApc - Remove an APC from a thread's queue
 * 
 * Ordinal: 135
 * 
 * BOOLEAN KeRemoveQueueApc(PKAPC Apc);
 */
static void HLE_KeRemoveQueueApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc_ptr = static_cast<GuestAddr>(args[0]);
    
    // Check if inserted
    u8 inserted = memory->read_u8(apc_ptr + 0x03);
    if (!inserted) {
        *result = 0;  // FALSE - wasn't in queue
        return;
    }
    
    // Mark as not inserted
    memory->write_u8(apc_ptr + 0x03, 0);
    
    // Note: The actual removal from the thread's queue would require
    // tracking which APCs are queued to which threads. For simplicity,
    // we just mark it as not inserted. The APC won't fire because
    // the thread will check this flag before executing.
    
    LOGD("KeRemoveQueueApc: removed APC at 0x%08X", apc_ptr);
    *result = 1;  // TRUE
}

/**
 * NtQueueApcThread - Queue a user APC to a thread
 * 
 * Ordinal: 205
 * 
 * NTSTATUS NtQueueApcThread(
 *   HANDLE ThreadHandle,          // arg[0] - Target thread
 *   PPS_APC_ROUTINE ApcRoutine,   // arg[1] - APC routine
 *   PVOID ApcRoutineContext,      // arg[2] - First argument
 *   PIO_STATUS_BLOCK ApcStatusBlock, // arg[3] - Second argument
 *   ULONG ApcReserved             // arg[4] - Third argument (reserved)
 * );
 */
static void HLE_NtQueueApcThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);
    GuestAddr arg1 = static_cast<GuestAddr>(args[3]);
    GuestAddr arg2 = static_cast<GuestAddr>(args[4]);
    
    LOGI("NtQueueApcThread: handle=0x%X, routine=0x%08X, context=0x%08X",
         handle, routine, context);
    
    ThreadScheduler* scheduler = get_thread_scheduler();
    
    if (!scheduler) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    GuestThread* thread = scheduler->get_thread_by_handle(handle);
    if (!thread) {
        *result = nt::STATUS_INVALID_HANDLE;
        return;
    }
    
    // Queue as user-mode APC
    scheduler->queue_apc(thread, routine, context, arg1, arg2, false);
    
    *result = nt::STATUS_SUCCESS;
}

/**
 * NtTestAlert - Check and clear alert status, delivering any pending APCs
 * 
 * Ordinal: 214
 */
static void HLE_NtTestAlert(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    ThreadScheduler* scheduler = get_thread_scheduler();
    
    if (!scheduler) {
        *result = nt::STATUS_SUCCESS;
        return;
    }
    
    GuestThread* current = scheduler->get_current_thread(0);
    if (!current) {
        *result = nt::STATUS_SUCCESS;
        return;
    }
    
    // Check for pending APCs
    if (current->has_pending_apcs()) {
        scheduler->process_pending_apcs(current);
        *result = nt::STATUS_USER_APC;
        return;
    }
    
    // Check if alerted
    if (current->alerted) {
        current->alerted = false;
        *result = nt::STATUS_ALERTED;
        return;
    }
    
    *result = nt::STATUS_SUCCESS;
}

/**
 * NtAlertThread - Alert a thread
 * 
 * Ordinal: 185
 */
static void HLE_NtAlertThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    
    ThreadScheduler* scheduler = get_thread_scheduler();
    
    if (!scheduler) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    GuestThread* thread = scheduler->get_thread_by_handle(handle);
    if (!thread) {
        *result = nt::STATUS_INVALID_HANDLE;
        return;
    }
    
    scheduler->alert_thread(thread);
    
    *result = nt::STATUS_SUCCESS;
}

//=============================================================================
// Event HLE Functions
//=============================================================================

/**
 * NtCreateEvent - Create an event object
 * 
 * Ordinal: 189
 */
static void HLE_NtCreateEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 access_mask = static_cast<u32>(args[1]);
    GuestAddr obj_attr = static_cast<GuestAddr>(args[2]);
    u32 event_type = static_cast<u32>(args[3]);
    u32 initial_state = static_cast<u32>(args[4]);
    
    LOGD("NtCreateEvent: type=%u, initial=%u", event_type, initial_state);
    
    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 handle = 0;
    u32 status = g_ktm->create_event(&handle, access_mask, obj_attr,
                                      static_cast<EventType>(event_type),
                                      initial_state != 0);
    
    if (status == nt::STATUS_SUCCESS && handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    *result = status;
}

/**
 * NtSetEvent - Signal an event
 * 
 * Ordinal: 210
 */
static void HLE_NtSetEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_state_ptr = static_cast<GuestAddr>(args[1]);
    
    s32 prev_state = 0;
    u32 status = g_ktm ? g_ktm->set_event(handle, &prev_state) : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_state_ptr) {
        memory->write_u32(prev_state_ptr, static_cast<u32>(prev_state));
    }
    
    *result = status;
}

/**
 * NtClearEvent - Clear (reset) an event
 * 
 * Ordinal: 188
 */
static void HLE_NtClearEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    *result = g_ktm ? g_ktm->clear_event(handle) : nt::STATUS_UNSUCCESSFUL;
}

/**
 * NtPulseEvent - Pulse an event (set then reset)
 * 
 * Ordinal: 206
 */
static void HLE_NtPulseEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_state_ptr = static_cast<GuestAddr>(args[1]);
    
    s32 prev_state = 0;
    u32 status = g_ktm ? g_ktm->pulse_event(handle, &prev_state) : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_state_ptr) {
        memory->write_u32(prev_state_ptr, static_cast<u32>(prev_state));
    }
    
    *result = status;
}

//=============================================================================
// Semaphore HLE Functions
//=============================================================================

/**
 * NtCreateSemaphore - Create a semaphore
 * 
 * Ordinal: 191
 */
static void HLE_NtCreateSemaphore(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 access_mask = static_cast<u32>(args[1]);
    GuestAddr obj_attr = static_cast<GuestAddr>(args[2]);
    s32 initial_count = static_cast<s32>(args[3]);
    s32 max_count = static_cast<s32>(args[4]);
    
    LOGD("NtCreateSemaphore: initial=%d, max=%d", initial_count, max_count);
    
    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 handle = 0;
    u32 status = g_ktm->create_semaphore(&handle, access_mask, obj_attr,
                                          initial_count, max_count);
    
    if (status == nt::STATUS_SUCCESS && handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    *result = status;
}

/**
 * NtReleaseSemaphore - Release a semaphore
 * 
 * Ordinal: 208
 */
static void HLE_NtReleaseSemaphore(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    s32 release_count = static_cast<s32>(args[1]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[2]);
    
    s32 prev_count = 0;
    u32 status = g_ktm ? g_ktm->release_semaphore(handle, release_count, &prev_count)
                       : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_count_ptr) {
        memory->write_u32(prev_count_ptr, static_cast<u32>(prev_count));
    }
    
    *result = status;
}

//=============================================================================
// Mutant (Mutex) HLE Functions
//=============================================================================

/**
 * NtCreateMutant - Create a mutant (mutex)
 * 
 * Ordinal: 190
 */
static void HLE_NtCreateMutant(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 access_mask = static_cast<u32>(args[1]);
    GuestAddr obj_attr = static_cast<GuestAddr>(args[2]);
    u32 initial_owner = static_cast<u32>(args[3]);
    
    LOGD("NtCreateMutant: initial_owner=%u", initial_owner);
    
    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    u32 handle = 0;
    u32 status = g_ktm->create_mutant(&handle, access_mask, obj_attr, initial_owner != 0);
    
    if (status == nt::STATUS_SUCCESS && handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }
    
    *result = status;
}

/**
 * NtReleaseMutant - Release a mutant
 * 
 * Ordinal: 207
 */
static void HLE_NtReleaseMutant(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr prev_count_ptr = static_cast<GuestAddr>(args[1]);
    
    s32 prev_count = 0;
    u32 status = g_ktm ? g_ktm->release_mutant(handle, false, &prev_count)
                       : nt::STATUS_UNSUCCESSFUL;
    
    if (status == nt::STATUS_SUCCESS && prev_count_ptr) {
        memory->write_u32(prev_count_ptr, static_cast<u32>(prev_count));
    }
    
    *result = status;
}

//=============================================================================
// Wait Functions
//=============================================================================

/**
 * NtWaitForSingleObject - Wait for object to be signaled
 * 
 * Ordinal: 217
 */
static void HLE_NtWaitForSingleObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    u32 alertable = static_cast<u32>(args[1]);
    GuestAddr timeout_ptr = static_cast<GuestAddr>(args[2]);
    
    LOGD("NtWaitForSingleObject: handle=0x%X, alertable=%u", handle, alertable);
    
    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    s64 timeout = 0;
    s64* timeout_p = nullptr;
    
    if (timeout_ptr) {
        timeout = static_cast<s64>(memory->read_u64(timeout_ptr));
        timeout_p = &timeout;
    }
    
    *result = g_ktm->wait_for_single_object(handle, alertable != 0, timeout_p);
}

/**
 * NtWaitForMultipleObjects - Wait for multiple objects
 * 
 * Ordinal: 218  
 */
static void HLE_NtWaitForMultipleObjects(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 count = static_cast<u32>(args[0]);
    GuestAddr handles_ptr = static_cast<GuestAddr>(args[1]);
    u32 wait_type = static_cast<u32>(args[2]);
    u32 alertable = static_cast<u32>(args[3]);
    GuestAddr timeout_ptr = static_cast<GuestAddr>(args[4]);
    
    LOGD("NtWaitForMultipleObjects: count=%u, type=%u", count, wait_type);
    
    if (!g_ktm || count == 0 || count > 64) {
        *result = nt::STATUS_INVALID_PARAMETER;
        return;
    }
    
    // Read handles
    std::vector<u32> handles(count);
    for (u32 i = 0; i < count; i++) {
        handles[i] = memory->read_u32(handles_ptr + i * 4);
    }
    
    s64 timeout = 0;
    s64* timeout_p = nullptr;
    
    if (timeout_ptr) {
        timeout = static_cast<s64>(memory->read_u64(timeout_ptr));
        timeout_p = &timeout;
    }
    
    *result = g_ktm->wait_for_multiple_objects(count, handles.data(),
                                                static_cast<WaitType>(wait_type),
                                                alertable != 0, timeout_p);
}

/**
 * KeDelayExecutionThread - Sleep
 * 
 * Ordinal: 40
 */
static void HLE_KeDelayExecutionThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 processor_mode = static_cast<u32>(args[0]);
    u32 alertable = static_cast<u32>(args[1]);
    GuestAddr interval_ptr = static_cast<GuestAddr>(args[2]);
    
    s64 interval = 0;
    s64* interval_p = nullptr;
    
    if (interval_ptr) {
        interval = static_cast<s64>(memory->read_u64(interval_ptr));
        interval_p = &interval;
    }
    
    *result = g_ktm ? g_ktm->delay_execution(alertable != 0, interval_p) : nt::STATUS_SUCCESS;
}

//=============================================================================
// Critical Section Functions
//=============================================================================

/**
 * RtlInitializeCriticalSection - Initialize a critical section
 * 
 * Ordinal: 277
 */
static void HLE_RtlInitializeCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    
    if (g_ktm) {
        g_ktm->init_critical_section(cs_ptr);
    }
    
    *result = nt::STATUS_SUCCESS;
}

/**
 * RtlInitializeCriticalSectionAndSpinCount - Initialize with spin count
 * 
 * Ordinal: 278
 */
static void HLE_RtlInitializeCriticalSectionAndSpinCount(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    u32 spin_count = static_cast<u32>(args[1]);
    
    *result = g_ktm ? g_ktm->init_critical_section_with_spin(cs_ptr, spin_count)
                    : nt::STATUS_SUCCESS;
}

/**
 * RtlEnterCriticalSection - Enter (acquire) a critical section
 * 
 * Ordinal: 274
 */
static void HLE_RtlEnterCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    *result = g_ktm ? g_ktm->enter_critical_section(cs_ptr) : nt::STATUS_SUCCESS;
}

/**
 * RtlLeaveCriticalSection - Leave (release) a critical section
 * 
 * Ordinal: 285
 */
static void HLE_RtlLeaveCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    *result = g_ktm ? g_ktm->leave_critical_section(cs_ptr) : nt::STATUS_SUCCESS;
}

/**
 * RtlTryEnterCriticalSection - Try to enter without blocking
 * 
 * Ordinal: 290
 */
static void HLE_RtlTryEnterCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    *result = g_ktm ? g_ktm->try_enter_critical_section(cs_ptr) : 0;
}

/**
 * RtlDeleteCriticalSection - Delete a critical section
 * 
 * Ordinal: 272
 */
static void HLE_RtlDeleteCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs_ptr = static_cast<GuestAddr>(args[0]);
    *result = g_ktm ? g_ktm->delete_critical_section(cs_ptr) : nt::STATUS_SUCCESS;
}

//=============================================================================
// Thread Local Storage Functions
//=============================================================================

/**
 * TlsAlloc - Allocate a TLS slot
 * 
 * Ordinal: 330
 */
static void HLE_KeTlsAlloc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    *result = g_ktm ? g_ktm->tls_alloc() : nt::TLS_OUT_OF_INDEXES;
}

/**
 * TlsFree - Free a TLS slot
 * 
 * Ordinal: 331
 */
static void HLE_KeTlsFree(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 index = static_cast<u32>(args[0]);
    *result = g_ktm ? g_ktm->tls_free(index) : 0;
}

/**
 * TlsGetValue - Get TLS value
 * 
 * Ordinal: 332
 */
static void HLE_KeTlsGetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 index = static_cast<u32>(args[0]);
    *result = g_ktm ? g_ktm->tls_get_value(index) : 0;
}

/**
 * TlsSetValue - Set TLS value
 * 
 * Ordinal: 333
 */
static void HLE_KeTlsSetValue(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 index = static_cast<u32>(args[0]);
    u64 value = args[1];
    *result = g_ktm ? g_ktm->tls_set_value(index, value) : 0;
}

//=============================================================================
// Handle Management
//=============================================================================

/**
 * NtClose - Close a handle
 * 
 * Ordinal: 187 (shared with file handles)
 */
static void HLE_NtCloseThreadHandle(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    *result = g_ktm ? g_ktm->close_handle(handle) : nt::STATUS_SUCCESS;
}

/**
 * NtDuplicateObject - Duplicate a handle
 * 
 * Ordinal: 192
 */
static void HLE_NtDuplicateObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 source_process = static_cast<u32>(args[0]);
    u32 source_handle = static_cast<u32>(args[1]);
    u32 target_process = static_cast<u32>(args[2]);
    GuestAddr target_handle_ptr = static_cast<GuestAddr>(args[3]);
    
    u32 target_handle = 0;
    u32 status = g_ktm ? g_ktm->duplicate_handle(source_handle, &target_handle)
                       : nt::STATUS_SUCCESS;
    
    if (status == nt::STATUS_SUCCESS && target_handle_ptr) {
        memory->write_u32(target_handle_ptr, target_handle);
    }
    
    *result = status;
}

//=============================================================================
// Work Queue HLE Functions
//=============================================================================

/**
 * ExInitializeWorkItem - Initialize a work queue item
 * 
 * Ordinal: 60
 * 
 * VOID ExInitializeWorkItem(
 *   PWORK_QUEUE_ITEM WorkItem,       // arg[0] - Work item to initialize
 *   PWORKER_THREAD_ROUTINE Routine,  // arg[1] - Worker routine
 *   PVOID Context                    // arg[2] - Context parameter
 * );
 * 
 * WORK_QUEUE_ITEM structure:
 * +0x00: LIST_ENTRY List (Flink, Blink)
 * +0x08: PWORKER_THREAD_ROUTINE WorkerRoutine
 * +0x0C: PVOID Parameter
 */
static void HLE_ExInitializeWorkItem(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr work_item = static_cast<GuestAddr>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);
    
    LOGI("ExInitializeWorkItem: item=0x%08X, routine=0x%08X, context=0x%08X",
         (u32)work_item, (u32)routine, (u32)context);
    
    // Initialize LIST_ENTRY to point to itself (empty/unlinked)
    memory->write_u32(work_item + 0x00, work_item);  // Flink = self
    memory->write_u32(work_item + 0x04, work_item);  // Blink = self
    memory->write_u32(work_item + 0x08, routine);    // WorkerRoutine
    memory->write_u32(work_item + 0x0C, context);    // Parameter
    
    // void function - no return value needed
}

/**
 * ExQueueWorkItem - Queue a work item to a system worker thread
 * 
 * Ordinal: 61
 * 
 * VOID ExQueueWorkItem(
 *   PWORK_QUEUE_ITEM WorkItem,  // arg[0] - Work item to queue
 *   WORK_QUEUE_TYPE QueueType   // arg[1] - Queue type (Critical, Delayed, HyperCritical)
 * );
 */
static void HLE_ExQueueWorkItem(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr work_item_addr = static_cast<GuestAddr>(args[0]);
    u32 queue_type = static_cast<u32>(args[1]);
    
    // Read the work item from guest memory
    WorkQueueItem item;
    item.list_flink = memory->read_u32(work_item_addr + 0x00);
    item.list_blink = memory->read_u32(work_item_addr + 0x04);
    item.worker_routine = memory->read_u32(work_item_addr + 0x08);
    item.parameter = memory->read_u32(work_item_addr + 0x0C);
    item.item_address = work_item_addr;
    
    LOGI("ExQueueWorkItem: item=0x%08X, routine=0x%08X, param=0x%08X, type=%u",
         (u32)work_item_addr, (u32)item.worker_routine, (u32)item.parameter, queue_type);
    
    // Validate routine pointer - must be in valid code range
    if (item.worker_routine == 0) {
        LOGW("ExQueueWorkItem: NULL routine pointer");
        return;
    }
    
    if (item.worker_routine < 0x80000000 || item.worker_routine >= 0xC0000000) {
        LOGW("ExQueueWorkItem: Invalid routine pointer 0x%08X", (u32)item.worker_routine);
        return;
    }
    
    // Clamp queue type to valid range
    if (queue_type >= static_cast<u32>(WorkQueueType::Maximum)) {
        queue_type = static_cast<u32>(WorkQueueType::Delayed);
    }
    
    // Queue to the work queue manager
    WorkQueueManager::instance().enqueue(static_cast<WorkQueueType>(queue_type), item);
    
    // void function - no return value needed
}

//=============================================================================
// Timer HLE Functions
//=============================================================================

/**
 * NtCreateTimer - Create a timer object
 *
 * Ordinal: 193
 */
static void HLE_NtCreateTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 access_mask = static_cast<u32>(args[1]);
    GuestAddr obj_attr = static_cast<GuestAddr>(args[2]);
    u32 timer_type = static_cast<u32>(args[3]);

    LOGD("NtCreateTimer: type=%u", timer_type);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    u32 handle = 0;
    u32 status = g_ktm->create_timer(&handle, access_mask, obj_attr, timer_type);

    if (status == nt::STATUS_SUCCESS && handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }

    *result = status;
}

/**
 * NtSetTimerEx / NtSetTimer - Set a timer
 *
 * Ordinal: 212
 */
static void HLE_NtSetTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr due_time_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr timer_apc_routine = static_cast<GuestAddr>(args[2]);
    GuestAddr timer_context = static_cast<GuestAddr>(args[3]);
    u32 resume = static_cast<u32>(args[4]);
    u32 period = static_cast<u32>(args[5]);
    GuestAddr prev_state_ptr = static_cast<GuestAddr>(args[6]);

    s64 due_time = 0;
    if (due_time_ptr) {
        due_time = static_cast<s64>(memory->read_u64(due_time_ptr));
    }

    LOGD("NtSetTimer: handle=0x%X, due=%lld, period=%u", handle, (long long)due_time, period);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    bool prev_state = false;
    u32 status = g_ktm->set_timer(handle, due_time, period,
                                   timer_apc_routine, timer_context,
                                   resume != 0, &prev_state);

    if (status == nt::STATUS_SUCCESS && prev_state_ptr) {
        memory->write_u32(prev_state_ptr, prev_state ? 1 : 0);
    }

    *result = status;
}

/**
 * NtCancelTimer - Cancel a timer
 *
 * Ordinal: 186
 */
static void HLE_NtCancelTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr was_set_ptr = static_cast<GuestAddr>(args[1]);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    bool was_set = false;
    u32 status = g_ktm->cancel_timer(handle, &was_set);

    if (status == nt::STATUS_SUCCESS && was_set_ptr) {
        memory->write_u32(was_set_ptr, was_set ? 1 : 0);
    }

    *result = status;
}

//=============================================================================
// I/O Completion Port HLE Functions
//=============================================================================

/**
 * NtCreateIoCompletion - Create an I/O completion port
 *
 * Ordinal: 196
 */
static void HLE_NtCreateIoCompletion(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 access_mask = static_cast<u32>(args[1]);
    GuestAddr obj_attr = static_cast<GuestAddr>(args[2]);
    u32 max_threads = static_cast<u32>(args[3]);

    LOGD("NtCreateIoCompletion: max_threads=%u", max_threads);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    u32 handle = 0;
    u32 status = g_ktm->create_io_completion(&handle, access_mask, obj_attr, max_threads);

    if (status == nt::STATUS_SUCCESS && handle_ptr) {
        memory->write_u32(handle_ptr, handle);
    }

    *result = status;
}

/**
 * NtSetIoCompletion - Post a packet to I/O completion port
 *
 * Ordinal: 211
 */
static void HLE_NtSetIoCompletion(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr key_context = static_cast<GuestAddr>(args[1]);
    GuestAddr apc_context = static_cast<GuestAddr>(args[2]);
    u32 status_code = static_cast<u32>(args[3]);
    u32 bytes = static_cast<u32>(args[4]);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    *result = g_ktm->set_io_completion(handle, key_context, apc_context, status_code, bytes);
}

/**
 * NtRemoveIoCompletion - Dequeue a packet from I/O completion port
 *
 * Ordinal: 197
 */
static void HLE_NtRemoveIoCompletion(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr key_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr apc_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr status_ptr = static_cast<GuestAddr>(args[3]);
    GuestAddr bytes_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr timeout_ptr = static_cast<GuestAddr>(args[5]);

    if (!g_ktm) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }

    s64 timeout = 0;
    s64* timeout_p = nullptr;
    if (timeout_ptr) {
        timeout = static_cast<s64>(memory->read_u64(timeout_ptr));
        timeout_p = &timeout;
    }

    IoCompletionPacket packet = {};
    u32 status = g_ktm->remove_io_completion(handle, &packet, timeout_p);

    if (status == nt::STATUS_SUCCESS) {
        if (key_ptr) memory->write_u32(key_ptr, packet.key_context);
        if (apc_ptr) memory->write_u32(apc_ptr, packet.apc_context);
        if (status_ptr) memory->write_u32(status_ptr, packet.status);
        if (bytes_ptr) memory->write_u32(bytes_ptr, packet.bytes_transferred);
    }

    *result = status;
}

//=============================================================================
// DPC (Deferred Procedure Call) HLE Functions
//=============================================================================

/**
 * KeInitializeDpc - Initialize a DPC object
 *
 * Ordinal: 107
 */
static void HLE_KeInitializeDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);

    LOGD("KeInitializeDpc: dpc=0x%08X, routine=0x%08X, context=0x%08X",
         dpc_ptr, routine, context);

    // KDPC structure layout (Xbox 360):
    // +0x00: Type (SHORT) = 19 (DpcObject)
    // +0x02: Importance (UCHAR)
    // +0x03: Number (UCHAR) - target processor
    // +0x04: DpcListEntry (LIST_ENTRY)
    // +0x0C: DeferredRoutine
    // +0x10: DeferredContext
    // +0x14: SystemArgument1
    // +0x18: SystemArgument2
    // +0x1C: DpcData (PVOID)

    memory->write_u16(dpc_ptr + 0x00, 19);        // Type = DpcObject
    memory->write_u8(dpc_ptr + 0x02, 1);           // Importance = Medium
    memory->write_u8(dpc_ptr + 0x03, 0);           // Number = 0 (any processor)
    memory->write_u32(dpc_ptr + 0x04, 0);          // DpcListEntry.Flink
    memory->write_u32(dpc_ptr + 0x08, 0);          // DpcListEntry.Blink
    memory->write_u32(dpc_ptr + 0x0C, routine);    // DeferredRoutine
    memory->write_u32(dpc_ptr + 0x10, context);    // DeferredContext
    memory->write_u32(dpc_ptr + 0x14, 0);          // SystemArgument1
    memory->write_u32(dpc_ptr + 0x18, 0);          // SystemArgument2
    memory->write_u32(dpc_ptr + 0x1C, 0);          // DpcData

    // void function
}

/**
 * KeInsertQueueDpc - Queue a DPC for execution
 *
 * Ordinal: 109
 */
static void HLE_KeInsertQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr system_arg1 = static_cast<GuestAddr>(args[1]);
    GuestAddr system_arg2 = static_cast<GuestAddr>(args[2]);

    // Store system arguments
    memory->write_u32(dpc_ptr + 0x14, system_arg1);
    memory->write_u32(dpc_ptr + 0x18, system_arg2);

    // Read routine and context
    GuestAddr routine = memory->read_u32(dpc_ptr + 0x0C);
    GuestAddr context = memory->read_u32(dpc_ptr + 0x10);

    LOGI("KeInsertQueueDpc: dpc=0x%08X, routine=0x%08X, ctx=0x%08X, arg1=0x%08X, arg2=0x%08X",
         dpc_ptr, routine, context, system_arg1, system_arg2);

    if (routine != 0) {
        KernelState::instance().queue_dpc(dpc_ptr, routine, context, system_arg1, system_arg2);
        *result = 1;  // TRUE - inserted
    } else {
        *result = 0;  // FALSE
    }
}

/**
 * KeRemoveQueueDpc - Remove a DPC from the queue
 *
 * Ordinal: 134
 */
static void HLE_KeRemoveQueueDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // We don't track individual DPCs for removal in our simplified model
    // Just return FALSE (not removed) - this is rarely called
    *result = 0;
}

/**
 * KeSetImportanceDpc - Set the importance level of a DPC
 *
 * Ordinal: 153
 */
static void HLE_KeSetImportanceDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc_ptr = static_cast<GuestAddr>(args[0]);
    u32 importance = static_cast<u32>(args[1]);

    memory->write_u8(dpc_ptr + 0x02, static_cast<u8>(importance));
    // void function
}

/**
 * KeSetTargetProcessorDpc - Set the target processor for a DPC
 *
 * Ordinal: 154
 */
static void HLE_KeSetTargetProcessorDpc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr dpc_ptr = static_cast<GuestAddr>(args[0]);
    u32 processor = static_cast<u32>(args[1]);

    memory->write_u8(dpc_ptr + 0x03, static_cast<u8>(processor));
    // void function
}

//=============================================================================
// Timer Ke-level HLE Functions
//=============================================================================

/**
 * KeInitializeTimerEx - Initialize a timer object in guest memory
 *
 * Ordinal: 111
 */
static void HLE_KeInitializeTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer_ptr = static_cast<GuestAddr>(args[0]);
    u32 timer_type = static_cast<u32>(args[1]);

    LOGD("KeInitializeTimerEx: timer=0x%08X, type=%u", timer_ptr, timer_type);

    // KTIMER structure (DispatcherHeader + timer fields)
    u8 type_val = (timer_type == 0) ? 0x08 : 0x09;  // Notification vs Synchronization
    memory->write_u8(timer_ptr + 0x00, type_val);
    memory->write_u8(timer_ptr + 0x01, 0);
    memory->write_u16(timer_ptr + 0x02, 0x0A);  // Size in dwords
    memory->write_u32(timer_ptr + 0x04, 0);      // SignalState = not signaled
    // void function
}

/**
 * KeSetTimerEx - Set a timer in guest memory
 *
 * Ordinal: 150
 */
static void HLE_KeSetTimerEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer_ptr = static_cast<GuestAddr>(args[0]);
    s64 due_time = static_cast<s64>(args[1]);
    u32 period = static_cast<u32>(args[2]);
    GuestAddr dpc_ptr = static_cast<GuestAddr>(args[3]);

    LOGD("KeSetTimerEx: timer=0x%08X, due=%lld, period=%u, dpc=0x%08X",
         timer_ptr, (long long)due_time, period, dpc_ptr);

    // Read previous signal state
    s32 prev_state = static_cast<s32>(memory->read_u32(timer_ptr + 0x04));

    // Reset signal state
    memory->write_u32(timer_ptr + 0x04, 0);

    // Queue the timer through KernelState
    u64 abs_due_time;
    if (due_time < 0) {
        // Relative time
        abs_due_time = KernelState::instance().system_time() + static_cast<u64>(-due_time);
    } else {
        abs_due_time = static_cast<u64>(due_time);
    }

    u64 period_100ns = static_cast<u64>(period) * 10000ULL;

    KernelState::instance().queue_timer(timer_ptr, abs_due_time, period_100ns, dpc_ptr);

    *result = (prev_state != 0) ? 1 : 0;  // TRUE if timer was already set
}

/**
 * KeCancelTimer - Cancel a timer
 *
 * Ordinal: 36
 */
static void HLE_KeCancelTimer(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr timer_ptr = static_cast<GuestAddr>(args[0]);

    bool was_set = KernelState::instance().cancel_timer(timer_ptr);

    // Clear signal state
    memory->write_u32(timer_ptr + 0x04, 0);

    *result = was_set ? 1 : 0;
}

//=============================================================================
// Additional Ordinal Aliases
// Some games import these functions under alternate ordinal numbers.
//=============================================================================

/**
 * NtWaitForSingleObjectEx - Wait for object with timeout (Ex variant)
 *
 * Ordinal: 32
 * Same signature as NtWaitForSingleObject; the "Ex" just adds alertable semantics
 * which our implementation already supports.
 */
static void HLE_NtWaitForSingleObjectEx(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Identical to NtWaitForSingleObject - our impl already handles alertable
    HLE_NtWaitForSingleObject(cpu, memory, args, result);
}

/**
 * RtlInitAnsiString - Initialize ANSI_STRING from char*
 *
 * Ordinal: 182
 *
 * VOID RtlInitAnsiString(
 *   PANSI_STRING DestString,  // arg[0] - pointer to ANSI_STRING { USHORT Length, USHORT MaxLength, PCHAR Buffer }
 *   PCSTR SourceString        // arg[1] - null-terminated source string (or NULL)
 * );
 */
static void HLE_RtlInitAnsiString_Alt(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr ansi_string = static_cast<GuestAddr>(args[0]);
    GuestAddr source = static_cast<GuestAddr>(args[1]);

    u16 length = 0;
    if (source) {
        while (memory->read_u8(source + length) != 0 && length < 0xFFFE) {
            length++;
        }
    }

    // ANSI_STRING: { USHORT Length, USHORT MaximumLength, PCHAR Buffer }
    memory->write_u16(ansi_string, length);
    memory->write_u16(ansi_string + 2, source ? (length + 1) : 0);
    memory->write_u32(ansi_string + 4, source);

    *result = 0;
}

//=============================================================================
// Registration
//=============================================================================

void register_xboxkrnl_threading(Kernel* kernel) {
    // Get thread manager
    g_ktm = get_kernel_thread_manager();
    
    auto& funcs = kernel->get_hle_functions();
    auto make_key = [](u32 module, u32 ordinal) -> u64 {
        return (static_cast<u64>(module) << 32) | ordinal;
    };
    
    // Work Queue functions
    funcs[make_key(0, 60)] = HLE_ExInitializeWorkItem;
    funcs[make_key(0, 61)] = HLE_ExQueueWorkItem;
    
    // Thread management
    funcs[make_key(0, 14)] = HLE_ExCreateThread;
    funcs[make_key(0, 216)] = HLE_NtTerminateThread;
    funcs[make_key(0, 215)] = HLE_NtSuspendThread;
    funcs[make_key(0, 209)] = HLE_NtResumeThread;
    funcs[make_key(0, 49)] = HLE_KeGetCurrentProcessorNumber;
    funcs[make_key(0, 51)] = HLE_KeGetCurrentThread;
    funcs[make_key(0, 221)] = HLE_NtYieldExecution;
    funcs[make_key(0, 84)] = HLE_KeSetAffinityThread;
    
    // APC (Asynchronous Procedure Call) functions
    funcs[make_key(0, 106)] = HLE_KeInitializeApc;
    funcs[make_key(0, 108)] = HLE_KeInsertQueueApc;
    funcs[make_key(0, 135)] = HLE_KeRemoveQueueApc;
    funcs[make_key(0, 185)] = HLE_NtAlertThread;
    funcs[make_key(0, 205)] = HLE_NtQueueApcThread;
    funcs[make_key(0, 214)] = HLE_NtTestAlert;
    
    // Events
    funcs[make_key(0, 189)] = HLE_NtCreateEvent;
    funcs[make_key(0, 210)] = HLE_NtSetEvent;
    funcs[make_key(0, 188)] = HLE_NtClearEvent;
    funcs[make_key(0, 206)] = HLE_NtPulseEvent;
    
    // Semaphores
    funcs[make_key(0, 191)] = HLE_NtCreateSemaphore;
    funcs[make_key(0, 208)] = HLE_NtReleaseSemaphore;
    
    // Mutants
    funcs[make_key(0, 190)] = HLE_NtCreateMutant;
    funcs[make_key(0, 207)] = HLE_NtReleaseMutant;
    
    // Wait functions
    funcs[make_key(0, 217)] = HLE_NtWaitForSingleObject;
    funcs[make_key(0, 218)] = HLE_NtWaitForMultipleObjects;
    funcs[make_key(0, 40)] = HLE_KeDelayExecutionThread;
    
    // Critical sections
    funcs[make_key(0, 277)] = HLE_RtlInitializeCriticalSection;
    funcs[make_key(0, 278)] = HLE_RtlInitializeCriticalSectionAndSpinCount;
    funcs[make_key(0, 274)] = HLE_RtlEnterCriticalSection;
    funcs[make_key(0, 285)] = HLE_RtlLeaveCriticalSection;
    funcs[make_key(0, 290)] = HLE_RtlTryEnterCriticalSection;
    funcs[make_key(0, 272)] = HLE_RtlDeleteCriticalSection;
    
    // TLS
    funcs[make_key(0, 330)] = HLE_KeTlsAlloc;
    funcs[make_key(0, 331)] = HLE_KeTlsFree;
    funcs[make_key(0, 332)] = HLE_KeTlsGetValue;
    funcs[make_key(0, 333)] = HLE_KeTlsSetValue;
    
    // Handle management
    funcs[make_key(0, 192)] = HLE_NtDuplicateObject;

    // Timers (Nt-level)
    funcs[make_key(0, 193)] = HLE_NtCreateTimer;
    funcs[make_key(0, 212)] = HLE_NtSetTimer;
    funcs[make_key(0, 186)] = HLE_NtCancelTimer;

    // Timers (Ke-level)
    funcs[make_key(0, 111)] = HLE_KeInitializeTimerEx;
    funcs[make_key(0, 150)] = HLE_KeSetTimerEx;
    funcs[make_key(0, 36)] = HLE_KeCancelTimer;

    // I/O Completion Ports
    funcs[make_key(0, 196)] = HLE_NtCreateIoCompletion;
    funcs[make_key(0, 211)] = HLE_NtSetIoCompletion;
    funcs[make_key(0, 197)] = HLE_NtRemoveIoCompletion;

    // DPC functions
    funcs[make_key(0, 107)] = HLE_KeInitializeDpc;
    funcs[make_key(0, 109)] = HLE_KeInsertQueueDpc;
    funcs[make_key(0, 134)] = HLE_KeRemoveQueueDpc;
    funcs[make_key(0, 153)] = HLE_KeSetImportanceDpc;
    funcs[make_key(0, 154)] = HLE_KeSetTargetProcessorDpc;

    // Alternate ordinal aliases - some games import under different ordinal numbers.
    // Only register if the ordinal is not already taken by another function.
    funcs[make_key(0, 5)]   = HLE_NtClearEvent;              // alt ordinal for NtClearEvent
    funcs[make_key(0, 11)]  = HLE_ExCreateThread;            // alt ordinal for ExCreateThread
    funcs[make_key(0, 26)]  = HLE_NtSetEvent;                // alt ordinal for NtSetEvent
    funcs[make_key(0, 32)]  = HLE_NtWaitForSingleObjectEx;   // NtWaitForSingleObjectEx
    funcs[make_key(0, 182)] = HLE_RtlInitAnsiString_Alt;     // alt ordinal for RtlInitAnsiString

    LOGI("Registered xboxkrnl.exe threading HLE functions (%zu total)", funcs.size());
}

} // namespace x360mu
