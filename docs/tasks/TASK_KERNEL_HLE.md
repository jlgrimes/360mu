# Task: Kernel HLE Expansion

## Project Context
You are working on 360μ, an Xbox 360 emulator for Android. Instead of emulating the Xbox 360 kernel at low level, we use HLE (High Level Emulation) to intercept and emulate kernel API calls.

## Your Assignment
Expand the kernel HLE to support the functions Black Ops needs to boot.

## Current State
- Kernel header at `native/src/kernel/kernel.h`
- Basic HLE at `native/src/kernel/hle/xboxkrnl.cpp`
- Extended HLE at `native/src/kernel/hle/xboxkrnl_extended.cpp`
- XAM HLE at `native/src/kernel/hle/xam.cpp`
- Most functions are stubs returning success

## Critical Functions to Implement

### 1. Memory Management (`xboxkrnl.cpp`)
```cpp
// NtAllocateVirtualMemory - CRITICAL
void HLE_NtAllocateVirtualMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr* base_addr = (GuestAddr*)args[0];  // In/out
    u64* region_size = (u64*)args[1];            // In/out
    u32 alloc_type = args[2];
    u32 protect = args[3];
    
    // Actually allocate in emulator memory
    // Track allocations
    // Return STATUS_SUCCESS
}

// MmAllocatePhysicalMemoryEx
// MmFreePhysicalMemory
// MmMapIoSpace
```

### 2. Threading (`xboxkrnl.cpp`)
```cpp
// ExCreateThread - CRITICAL
void HLE_ExCreateThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = args[0];
    u32 stack_size = args[1];
    GuestAddr thread_id_ptr = args[2];
    GuestAddr start_addr = args[3];
    GuestAddr start_param = args[4];
    u32 create_flags = args[5];
    
    // Create new thread context
    // Set up stack
    // Schedule for execution
}

// KeWaitForSingleObject - CRITICAL
// KeSetEvent
// KeResetEvent
// RtlInitializeCriticalSection
// RtlEnterCriticalSection
// RtlLeaveCriticalSection
```

### 3. File I/O (`xboxkrnl.cpp`)
```cpp
// NtCreateFile - CRITICAL
void HLE_NtCreateFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr handle_ptr = args[0];
    u32 desired_access = args[1];
    GuestAddr obj_attr_ptr = args[2];
    GuestAddr io_status_ptr = args[3];
    
    // Parse OBJECT_ATTRIBUTES to get filename
    // Map Xbox path to host path (game:\, hdd:\, etc.)
    // Open file, create handle
}

// NtReadFile
// NtWriteFile
// NtQueryInformationFile
// NtSetInformationFile
// NtClose
```

### 4. XAM (Xbox Auxiliary Methods) (`xam.cpp`)
```cpp
// XamUserGetSigninState - Returns signed-in status
// XamUserGetXUID - Returns user ID
// XamContentCreate - DLC/Save data
// XamShowMessageBoxUI - Can stub to return OK
// XNetStartup - Network init (stub)
```

## Xbox 360 Path Mapping
```
game:\        → ISO/folder mount point
hdd:\         → Save data directory  
dvd:\         → Same as game:\
cache:\       → Temp directory
flash:\       → System flash (stub)
\\Device\Harddisk0\Partition1\ → hdd:\
```

## Key Data Structures
```cpp
// OBJECT_ATTRIBUTES
struct OBJECT_ATTRIBUTES {
    be_u32 root_directory;
    be_u32 object_name_ptr;  // Points to ANSI_STRING
    be_u32 attributes;
};

// ANSI_STRING
struct ANSI_STRING {
    be_u16 length;
    be_u16 max_length;
    be_u32 buffer_ptr;
};

// IO_STATUS_BLOCK
struct IO_STATUS_BLOCK {
    be_u32 status;
    be_u32 information;
};
```

## Build & Test
```bash
cd native/build
cmake ..
make -j4

# Test with XEX loading:
./xex_test path/to/default.xex --trace-hle
```

## Reference
- Xenia kernel: https://github.com/xenia-project/xenia/tree/master/src/xenia/kernel
- Xbox 360 SDK headers (search online)
- ReactOS (Windows kernel reference)

## Success Criteria
1. Can create threads that execute
2. Can allocate/free memory
3. Can open and read game files
4. Can handle basic synchronization (events, critical sections)
5. Black Ops gets past initial HLE calls without crashing

