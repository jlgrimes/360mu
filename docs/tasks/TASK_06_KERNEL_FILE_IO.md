# Task: Kernel File I/O HLE

## Status: ✅ COMPLETED

## Priority: 🔴 CRITICAL (Blocking)

## Estimated Time: 2-3 weeks

## Dependencies: TASK_01_FILE_SYSTEM

---

## Objective

Implement High-Level Emulation of Xbox 360 kernel file I/O functions so games can read their data files.

---

## What To Build

### Location

- `native/src/kernel/hle/xboxkrnl_io.cpp`

---

## Functions to Implement

### 1. NtCreateFile

```cpp
// Opens or creates a file
// Called constantly by games to load assets
NTSTATUS HLE_NtCreateFile(
    HANDLE* FileHandle,           // OUT: file handle
    ACCESS_MASK DesiredAccess,    // Access rights
    OBJECT_ATTRIBUTES* ObjAttr,   // Contains filename
    IO_STATUS_BLOCK* IoStatus,    // OUT: status
    LARGE_INTEGER* AllocSize,     // Initial size (create)
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,      // CREATE_NEW, OPEN_EXISTING, etc.
    ULONG CreateOptions
) {
    // 1. Extract filename from OBJECT_ATTRIBUTES
    std::string path = read_unicode_string(ObjAttr->ObjectName);

    // 2. Translate Xbox path to VFS path
    std::string translated = vfs_->translate_path(path);

    // 3. Open via VFS
    FileHandle handle = vfs_->open_file(translated, DesiredAccess);
    if (handle == INVALID_HANDLE) {
        return STATUS_NO_SUCH_FILE;
    }

    // 4. Store handle mapping
    *FileHandle = kernel_->create_handle(handle);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = FILE_OPENED;

    return STATUS_SUCCESS;
}
```

### 2. NtReadFile

```cpp
// Read data from file
NTSTATUS HLE_NtReadFile(
    HANDLE FileHandle,
    HANDLE Event,                 // Optional event for async
    void* ApcRoutine,            // APC callback (ignore)
    void* ApcContext,
    IO_STATUS_BLOCK* IoStatus,
    void* Buffer,                // OUT: data buffer
    ULONG Length,                // Bytes to read
    LARGE_INTEGER* ByteOffset    // File offset
) {
    // 1. Get internal file handle
    auto file = kernel_->get_file(FileHandle);
    if (!file) {
        return STATUS_INVALID_HANDLE;
    }

    // 2. Seek if offset provided
    if (ByteOffset) {
        vfs_->seek_file(file, ByteOffset->QuadPart, SEEK_SET);
    }

    // 3. Read data
    size_t bytes_read = vfs_->read_file(file, Buffer, Length);

    // 4. Update status
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = bytes_read;

    // 5. Signal event if provided (for async)
    if (Event) {
        kernel_->set_event(Event);
    }

    return STATUS_SUCCESS;
}
```

### 3. NtWriteFile

```cpp
NTSTATUS HLE_NtWriteFile(
    HANDLE FileHandle,
    HANDLE Event,
    void* ApcRoutine,
    void* ApcContext,
    IO_STATUS_BLOCK* IoStatus,
    const void* Buffer,
    ULONG Length,
    LARGE_INTEGER* ByteOffset
) {
    auto file = kernel_->get_file(FileHandle);
    if (!file) {
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset) {
        vfs_->seek_file(file, ByteOffset->QuadPart, SEEK_SET);
    }

    size_t written = vfs_->write_file(file, Buffer, Length);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = written;

    return STATUS_SUCCESS;
}
```

### 4. NtQueryInformationFile

```cpp
NTSTATUS HLE_NtQueryInformationFile(
    HANDLE FileHandle,
    IO_STATUS_BLOCK* IoStatus,
    void* FileInfo,
    ULONG Length,
    FILE_INFORMATION_CLASS InfoClass
) {
    auto file = kernel_->get_file(FileHandle);

    switch (InfoClass) {
        case FileStandardInformation: {
            auto info = (FILE_STANDARD_INFORMATION*)FileInfo;
            info->AllocationSize.QuadPart = vfs_->get_file_size(file);
            info->EndOfFile.QuadPart = vfs_->get_file_size(file);
            info->NumberOfLinks = 1;
            info->DeletePending = FALSE;
            info->Directory = FALSE;
            break;
        }

        case FilePositionInformation: {
            auto info = (FILE_POSITION_INFORMATION*)FileInfo;
            info->CurrentByteOffset.QuadPart = vfs_->get_position(file);
            break;
        }

        case FileNetworkOpenInformation: {
            auto info = (FILE_NETWORK_OPEN_INFORMATION*)FileInfo;
            info->AllocationSize.QuadPart = vfs_->get_file_size(file);
            info->EndOfFile.QuadPart = vfs_->get_file_size(file);
            // ... timestamps
            break;
        }
    }

    return STATUS_SUCCESS;
}
```

### 5. NtSetInformationFile

```cpp
NTSTATUS HLE_NtSetInformationFile(
    HANDLE FileHandle,
    IO_STATUS_BLOCK* IoStatus,
    void* FileInfo,
    ULONG Length,
    FILE_INFORMATION_CLASS InfoClass
) {
    auto file = kernel_->get_file(FileHandle);

    switch (InfoClass) {
        case FilePositionInformation: {
            auto info = (FILE_POSITION_INFORMATION*)FileInfo;
            vfs_->seek_file(file, info->CurrentByteOffset.QuadPart, SEEK_SET);
            break;
        }

        case FileEndOfFileInformation: {
            auto info = (FILE_END_OF_FILE_INFORMATION*)FileInfo;
            vfs_->truncate_file(file, info->EndOfFile.QuadPart);
            break;
        }
    }

    return STATUS_SUCCESS;
}
```

### 6. NtQueryDirectoryFile

```cpp
NTSTATUS HLE_NtQueryDirectoryFile(
    HANDLE FileHandle,
    HANDLE Event,
    void* ApcRoutine,
    void* ApcContext,
    IO_STATUS_BLOCK* IoStatus,
    void* FileInfo,
    ULONG Length,
    FILE_INFORMATION_CLASS InfoClass,
    ULONG QueryFlags,
    UNICODE_STRING* FileName,      // Search pattern (e.g., "*.xex")
    BOOLEAN RestartScan
) {
    auto dir = kernel_->get_file(FileHandle);
    std::string pattern = FileName ? read_unicode_string(FileName) : "*";

    auto entries = vfs_->query_directory(dir, pattern);

    // Fill FileInfo buffer with directory entries
    u8* ptr = (u8*)FileInfo;
    for (auto& entry : entries) {
        auto info = (FILE_DIRECTORY_INFORMATION*)ptr;
        info->NextEntryOffset = sizeof(FILE_DIRECTORY_INFORMATION) + entry.name.size() * 2;
        info->FileNameLength = entry.name.size() * 2;
        info->EndOfFile.QuadPart = entry.size;
        info->FileAttributes = entry.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        write_unicode_string(info->FileName, entry.name);

        ptr += info->NextEntryOffset;
        if (ptr - (u8*)FileInfo >= Length) break;
    }

    return STATUS_SUCCESS;
}
```

### 7. NtClose

```cpp
NTSTATUS HLE_NtClose(HANDLE Handle) {
    return kernel_->close_handle(Handle);
}
```

---

## Data Structures

```cpp
// Unicode string (from Xbox kernel)
struct UNICODE_STRING {
    USHORT Length;        // Current length in bytes
    USHORT MaximumLength; // Buffer size in bytes
    PWSTR Buffer;         // Pointer to wide string
};

struct OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING* ObjectName;
    ULONG Attributes;
    void* SecurityDescriptor;
    void* SecurityQualityOfService;
};

struct IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

// Status codes
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr NTSTATUS STATUS_INVALID_HANDLE = 0xC0000008;
constexpr NTSTATUS STATUS_END_OF_FILE = 0xC0000011;
```

---

## Export Registration

```cpp
void Kernel::register_file_io_exports() {
    exports_["NtCreateFile"] = {0x00000077, HLE_NtCreateFile};
    exports_["NtReadFile"] = {0x000000DA, HLE_NtReadFile};
    exports_["NtWriteFile"] = {0x00000112, HLE_NtWriteFile};
    exports_["NtClose"] = {0x00000019, HLE_NtClose};
    exports_["NtQueryInformationFile"] = {0x000000E0, HLE_NtQueryInformationFile};
    exports_["NtSetInformationFile"] = {0x000000FC, HLE_NtSetInformationFile};
    exports_["NtQueryDirectoryFile"] = {0x000000DE, HLE_NtQueryDirectoryFile};
    exports_["NtQueryFullAttributesFile"] = {0x000000E1, HLE_NtQueryFullAttributesFile};
}
```

---

## Test Cases

```cpp
TEST(KernelFileIO, CreateFile) {
    Kernel kernel;
    VirtualFileSystem vfs;
    vfs.mount_folder("testdata/", "game:");
    kernel.set_vfs(&vfs);

    HANDLE handle;
    OBJECT_ATTRIBUTES attr = make_object_attr("\\Device\\game\\test.txt");
    IO_STATUS_BLOCK io;

    NTSTATUS status = kernel.syscall_NtCreateFile(
        &handle, GENERIC_READ, &attr, &io, nullptr,
        0, FILE_SHARE_READ, FILE_OPEN, 0
    );

    EXPECT_EQ(status, STATUS_SUCCESS);
    EXPECT_NE(handle, INVALID_HANDLE);
}

TEST(KernelFileIO, ReadFile) {
    // ... setup ...

    u8 buffer[100];
    IO_STATUS_BLOCK io;

    NTSTATUS status = kernel.syscall_NtReadFile(
        handle, nullptr, nullptr, nullptr,
        &io, buffer, 100, nullptr
    );

    EXPECT_EQ(status, STATUS_SUCCESS);
    EXPECT_GT(io.Information, 0);
}
```

---

## Do NOT Touch

- VFS implementation (separate task)
- Thread management (separate task)
- Memory management (separate task)
- GPU/Audio code

---

## Success Criteria

1. ✅ NtCreateFile opens files from mounted VFS - **IMPLEMENTED**
2. ✅ NtReadFile reads file contents correctly - **IMPLEMENTED**
3. ✅ NtQueryInformationFile returns file size - **IMPLEMENTED**
4. ✅ NtQueryDirectoryFile lists directory contents - **IMPLEMENTED**
5. ✅ NtClose releases handles properly - **IMPLEMENTED**

## Implementation Summary

The following files were created/modified:

- **NEW**: `native/src/kernel/hle/xboxkrnl_io.cpp` - Complete file I/O HLE implementation
- **NEW**: `native/tests/kernel/test_file_io.cpp` - Unit tests for file I/O
- **MODIFIED**: `native/src/kernel/kernel.h` - Added function declarations
- **MODIFIED**: `native/src/kernel/kernel.cpp` - Integrated file I/O initialization
- **MODIFIED**: `native/CMakeLists.txt` - Added new source files

### Features Implemented:

- NtCreateFile with proper path translation and VFS integration
- NtReadFile with byte offset support
- NtWriteFile with byte offset support
- NtQueryInformationFile for FileBasicInformation, FileStandardInformation, FilePositionInformation, FileNetworkOpenInformation
- NtSetInformationFile for FilePositionInformation, FileEndOfFileInformation
- NtQueryDirectoryFile with pattern matching and directory enumeration state
- NtQueryFullAttributesFile for checking file attributes by path
- NtClose with proper handle cleanup
- Unicode string support for OBJECT_ATTRIBUTES path reading
- Xbox 360 path format translation (game:, dvd:, hdd:, cache:, \Device\ paths)

---

_This task handles kernel file I/O only. The underlying VFS is implemented separately._
