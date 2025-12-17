# Task: File System Implementation

## Priority: 🔴 CRITICAL (Blocking)

## Estimated Time: 2-4 weeks

## Dependencies: None

---

## Objective

Implement ISO 9660 and STFS file system support so the emulator can read Xbox 360 game discs and packages.

---

## What To Build

### Location

- `native/src/kernel/filesystem/`

### Files to Create/Modify

1. **`iso_device.cpp`** - ISO 9660 disc image mounting
2. **`stfs_device.cpp`** - STFS package format (LIVE/PIRS/CON)
3. **`vfs.cpp`** - Virtual file system routing

---

## Specific Implementation

### 1. ISO 9660 Parser (`iso_device.cpp`)

```cpp
class IsoDevice : public VfsDevice {
public:
    Status mount(const std::string& iso_path);

    // Required methods
    FileHandle open(const std::string& path, u32 access);
    size_t read(FileHandle handle, void* buffer, size_t size);
    void close(FileHandle handle);
    bool exists(const std::string& path);
    std::vector<DirEntry> list_directory(const std::string& path);
    u64 get_file_size(FileHandle handle);

private:
    // ISO structures
    struct PrimaryVolumeDescriptor {
        char system_id[32];
        char volume_id[32];
        u32 volume_space_size;
        u16 logical_block_size;
        u32 path_table_size;
        u32 path_table_lba;
        u32 root_dir_record_lba;
    };

    struct DirectoryRecord {
        u8 length;
        u8 ext_attr_length;
        u32 extent_lba;
        u32 data_length;
        u8 date_time[7];
        u8 flags;
        u8 interleave_unit;
        u8 interleave_gap;
        u16 volume_seq;
        u8 name_length;
        char name[]; // Variable length
    };

    FILE* iso_file_;
    PrimaryVolumeDescriptor pvd_;
    std::map<std::string, DirectoryRecord> file_cache_;
};
```

### 2. Key Functions to Implement

```cpp
// Read primary volume descriptor at sector 16
Status IsoDevice::read_pvd() {
    fseek(iso_file_, 16 * 2048, SEEK_SET);
    // Read and parse PVD
    // Volume descriptor starts with type (1 = PVD)
    // Then "CD001" identifier
}

// Parse directory records
void IsoDevice::parse_directory(u32 lba, u32 size, const std::string& parent_path) {
    // Read sector at LBA
    // Parse each DirectoryRecord
    // Recursively parse subdirectories
    // Cache file locations
}

// Read file data
size_t IsoDevice::read(FileHandle handle, void* buffer, size_t size) {
    // Seek to file's LBA * 2048 + current position
    // Read requested bytes
    // Handle sector boundaries
}
```

### 3. STFS Package Support (`stfs_device.cpp`)

Xbox 360 downloadable content uses STFS format:

```cpp
struct StfsHeader {
    u32 magic;           // 'LIVE', 'PIRS', or 'CON '
    u8 signature[0x228]; // RSA signature (can ignore)
    // ... license info
    u32 header_size;
    u32 content_type;
    u32 metadata_version;
    u64 content_size;
    u32 title_id;
    // ... more metadata
};

struct StfsVolumeDescriptor {
    u8 hash_table_block_count[3];
    u8 allocated_block_count[3];
    u8 file_table_block_num[3];
    // ...
};
```

### 4. VFS Integration

Update `vfs.cpp` to route paths:

```cpp
Status VirtualFileSystem::mount_iso(const std::string& iso_path, const std::string& mount_point) {
    auto device = std::make_unique<IsoDevice>();
    Status s = device->mount(iso_path);
    if (s != Status::Ok) return s;

    mounts_[mount_point] = std::move(device);
    return Status::Ok;
}

std::string VirtualFileSystem::translate_path(const std::string& xbox_path) {
    // "game:\\maps\\mp_nuketown.ff" -> find in mounted ISO
    // "cache:\\" -> temp directory
    // "hdd:\\" -> local storage
}
```

---

## Xbox 360 Path Conventions

| Xbox Path | Meaning            |
| --------- | ------------------ |
| `game:\`  | Current disc/title |
| `dvd:\`   | DVD drive root     |
| `hdd:\`   | Hard drive         |
| `cache:\` | Temp cache         |
| `title:\` | Title storage      |

---

## Test Cases to Write

```cpp
TEST(FileSystemTest, MountIso) {
    VirtualFileSystem vfs;
    EXPECT_EQ(vfs.mount_iso("test.iso", "game:"), Status::Ok);
}

TEST(FileSystemTest, ReadFile) {
    VirtualFileSystem vfs;
    vfs.mount_iso("test.iso", "game:");

    auto handle = vfs.open_file("game:\\default.xex", FileAccess::Read);
    EXPECT_NE(handle, INVALID_HANDLE);

    std::vector<u8> buffer(4);
    vfs.read_file(handle, buffer.data(), 4);
    EXPECT_EQ(buffer[0], 'X'); // XEX magic
}

TEST(FileSystemTest, ListDirectory) {
    VirtualFileSystem vfs;
    vfs.mount_iso("test.iso", "game:");

    auto entries = vfs.query_directory("game:\\*");
    EXPECT_GT(entries.size(), 0);
}
```

---

## Do NOT Touch

- GPU code (`src/gpu/`)
- JIT compiler (`src/cpu/jit/`)
- Audio code (`src/apu/`)
- Interpreter (`src/cpu/xenon/interpreter*.cpp`)

---

## Success Criteria

1. ✅ Can mount an ISO file
2. ✅ Can list files in root directory
3. ✅ Can read `default.xex` from mounted ISO
4. ✅ Can seek and read partial files
5. ✅ Path translation works (`game:\` → mounted ISO)

---

## Reference

- ISO 9660 spec: ECMA-119
- Xbox 360 STFS: Free60 wiki documentation
- Xenia's `vfs/` implementation for reference

---

_This task has no dependencies on other tasks and can be completed independently._
