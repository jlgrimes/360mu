# Task: Fix LZX Decompression

## Priority: CRITICAL

## Problem

The LZX decompressor in `native/src/kernel/xex_crypto.cpp` is a stub that does NOT actually decompress data. It just copies bytes, which breaks any XEX file using LZX compression (compression_type == 2).

Current broken code at lines 496-506:

```cpp
Status LzxDecompressor::decompress_verbatim_block(u32 uncompressed_size) {
    // Simplified - real implementation needs full Huffman table building
    LOGD("LZX verbatim block: %u bytes", uncompressed_size);

    // For now, just copy literals (won't actually work for real compressed data)
    for (u32 i = 0; i < uncompressed_size && bits_left_ >= 8; i++) {
        u8 lit = static_cast<u8>(read_bits(8));
        window_[window_pos_++ & (window_size_ - 1)] = lit;
    }

    return Status::Ok;
}
```

## Solution

Integrate `libmspack` - a well-tested BSD-licensed library that handles Microsoft's LZX compression format (same as Xbox 360 uses).

## Implementation Steps

### Step 1: Add libmspack source files

Create directory `native/third_party/mspack/` and add these files from libmspack (https://github.com/kyz/libmspack):

Required files:

- `lzxd.c` - LZX decompressor
- `mspack.h` - Main header
- `lzx.h` - LZX definitions
- `system.h` - System abstraction
- `readbits.h` - Bit reading macros

### Step 2: Update CMakeLists.txt

Add to `native/CMakeLists.txt` after line ~46:

```cmake
# Third-party: libmspack for LZX decompression
set(MSPACK_SOURCES
    third_party/mspack/lzxd.c
)

# mspack needs these defines
add_definitions(-DHAVE_INTTYPES_H=1)
```

Then add `${MSPACK_SOURCES}` to the library target.

### Step 3: Create mspack memory wrapper

In `native/src/kernel/xex_crypto.cpp`, add a memory-based mspack system interface before the LzxDecompressor class:

```cpp
#include "../third_party/mspack/mspack.h"

// Memory-based mspack system for LZX decompression
struct MemoryFile {
    const u8* data;
    u32 size;
    u32 pos;
};

static struct mspack_file* mem_open(struct mspack_system* self, const char* filename, int mode) {
    (void)self; (void)filename; (void)mode;
    return nullptr; // Not used - we use alloc directly
}

static void mem_close(struct mspack_file* file) {
    (void)file;
}

static int mem_read(struct mspack_file* file, void* buffer, int bytes) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    int avail = mf->size - mf->pos;
    int to_read = (bytes < avail) ? bytes : avail;
    if (to_read > 0) {
        memcpy(buffer, mf->data + mf->pos, to_read);
        mf->pos += to_read;
    }
    return to_read;
}

static int mem_write(struct mspack_file* file, void* buffer, int bytes) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    memcpy(const_cast<u8*>(mf->data) + mf->pos, buffer, bytes);
    mf->pos += bytes;
    return bytes;
}

static int mem_seek(struct mspack_file* file, off_t offset, int mode) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    switch (mode) {
        case MSPACK_SYS_SEEK_START: mf->pos = offset; break;
        case MSPACK_SYS_SEEK_CUR: mf->pos += offset; break;
        case MSPACK_SYS_SEEK_END: mf->pos = mf->size + offset; break;
    }
    return 0;
}

static off_t mem_tell(struct mspack_file* file) {
    return reinterpret_cast<MemoryFile*>(file)->pos;
}

static void mem_msg(struct mspack_file* file, const char* format, ...) {
    (void)file; (void)format;
}

static void* mem_alloc(struct mspack_system* self, size_t bytes) {
    (void)self;
    return malloc(bytes);
}

static void mem_free(void* ptr) {
    free(ptr);
}

static void mem_copy(void* src, void* dest, size_t bytes) {
    memcpy(dest, src, bytes);
}

static struct mspack_system mem_system = {
    mem_open, mem_close, mem_read, mem_write,
    mem_seek, mem_tell, mem_msg, mem_alloc, mem_free, mem_copy, nullptr
};
```

### Step 4: Replace the decompress method

Replace the entire `LzxDecompressor::decompress` method (around line 448-494) with:

```cpp
Status LzxDecompressor::decompress(const u8* src, u32 src_size,
                                    u8* dst, u32 dst_size) {
    // Create memory files for input and output
    MemoryFile input_file = { src, src_size, 0 };
    MemoryFile output_file = { dst, dst_size, 0 };

    // Initialize LZX decompressor
    // Xbox 360 uses window_bits typically 15-21, default to 17 (128KB window)
    struct lzxd_stream* lzx = lzxd_init(
        &mem_system,
        reinterpret_cast<struct mspack_file*>(&input_file),
        reinterpret_cast<struct mspack_file*>(&output_file),
        window_bits_,
        0,              // reset_interval (0 = no reset)
        4096,           // input buffer size
        dst_size,       // output length
        0               // is_delta (0 = normal LZX)
    );

    if (!lzx) {
        LOGE("Failed to initialize LZX decompressor");
        return Status::Error;
    }

    // Decompress
    int result = lzxd_decompress(lzx, dst_size);
    lzxd_free(lzx);

    if (result != MSPACK_ERR_OK) {
        LOGE("LZX decompression failed with error %d", result);
        return Status::Error;
    }

    LOGI("LZX decompression successful: %u -> %u bytes", src_size, dst_size);
    return Status::Ok;
}
```

### Step 5: Remove old stub methods

Delete these methods as they're no longer needed:

- `decompress_verbatim_block` (lines 496-507)
- `decompress_aligned_block` (lines 509-512)
- `decompress_uncompressed_block` (lines 514-546)
- All the bitstream reading methods (`init_bits`, `read_bits`, `peek_bits`)

### Step 6: Update header file

In `native/src/kernel/xex_crypto.h`, simplify the LzxDecompressor class:

```cpp
class LzxDecompressor {
public:
    LzxDecompressor();
    ~LzxDecompressor();

    Status initialize(u32 window_bits);
    void reset();

    Status decompress(const u8* src, u32 src_size,
                      u8* dst, u32 dst_size);

private:
    u32 window_bits_ = 17;  // Default 128KB window
    u32 window_size_ = 0;
};
```

## Testing

1. Find a game that uses LZX compression (compression_type == 2 in XEX header)
2. Attempt to load it - should now decompress correctly
3. Verify the PE image has valid MZ header after decompression
4. Check logs for "LZX decompression successful" message

## Files to Modify

- `native/CMakeLists.txt` - Add mspack sources
- `native/src/kernel/xex_crypto.cpp` - Replace LZX implementation
- `native/src/kernel/xex_crypto.h` - Simplify LzxDecompressor class
- `native/third_party/mspack/` - Add libmspack files (new directory)

## Dependencies

None - this task is independent of other blockers.
