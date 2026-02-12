/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX Cryptography Implementation
 * 
 * Uses mbedTLS for AES-128 and SHA-1 operations.
 * mbedTLS is lightweight and works well on Android.
 */

#include "xex_crypto.h"
#include <cstring>
#include <algorithm>
#include <memory>
#include <cstdlib>

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

// libmspack for LZX decompression
extern "C" {
#include "../../third_party/mspack/mspack.h"
}

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-crypto"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[CRYPTO] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[CRYPTO ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// AES-128 Implementation using mbedTLS
//=============================================================================

// Internal implementation class (not exposed in header)
class Aes128Impl {
public:
    Aes128Impl() {
        mbedtls_aes_init(&ctx_);
    }
    
    ~Aes128Impl() {
        mbedtls_aes_free(&ctx_);
    }
    
    void set_key(const u8* key) {
        memcpy(key_, key, 16);
        // Set decryption key
        mbedtls_aes_setkey_dec(&ctx_, key_, 128);
    }
    
    void decrypt_block(u8* data) {
        mbedtls_aes_crypt_ecb(&ctx_, MBEDTLS_AES_DECRYPT, data, data);
    }
    
    void decrypt_ecb(u8* data, u32 size) {
        // ECB mode decrypts each 16-byte block independently
        for (u32 i = 0; i < size; i += 16) {
            mbedtls_aes_crypt_ecb(&ctx_, MBEDTLS_AES_DECRYPT, data + i, data + i);
        }
    }
    
    void decrypt_cbc(u8* data, u32 size, const u8* iv) {
        // Need a mutable copy of IV (mbedTLS modifies it during operation)
        u8 iv_copy[16];
        memcpy(iv_copy, iv, 16);
        mbedtls_aes_crypt_cbc(&ctx_, MBEDTLS_AES_DECRYPT, size, iv_copy, data, data);
    }
    
private:
    mbedtls_aes_context ctx_;
    u8 key_[16] = {0};
};

// Thread-local storage for AES implementation
static thread_local std::unique_ptr<Aes128Impl> tls_aes_impl;

void Aes128::set_key(const u8* key) {
    if (!tls_aes_impl) {
        tls_aes_impl = std::make_unique<Aes128Impl>();
    }
    tls_aes_impl->set_key(key);
}

void Aes128::decrypt_block(u8* data) {
    if (tls_aes_impl) {
        tls_aes_impl->decrypt_block(data);
    }
}

void Aes128::decrypt_cbc(u8* data, u32 size, const u8* iv) {
    if (tls_aes_impl) {
        tls_aes_impl->decrypt_cbc(data, size, iv);
    }
}

void Aes128::decrypt_ecb(u8* data, u32 size) {
    if (tls_aes_impl) {
        tls_aes_impl->decrypt_ecb(data, size);
    }
}

//=============================================================================
// SHA-1 Implementation using mbedTLS
//=============================================================================

// Internal implementation class (not exposed in header)
class Sha1Impl {
public:
    Sha1Impl() {
        mbedtls_sha1_init(&ctx_);
        mbedtls_sha1_starts(&ctx_);
    }
    
    ~Sha1Impl() {
        mbedtls_sha1_free(&ctx_);
    }
    
    void reset() {
        mbedtls_sha1_starts(&ctx_);
    }
    
    void update(const u8* data, u32 size) {
        mbedtls_sha1_update(&ctx_, data, size);
    }
    
    void finalize(u8* hash) {
        mbedtls_sha1_finish(&ctx_, hash);
    }
    
private:
    mbedtls_sha1_context ctx_;
};

// Thread-local storage for SHA-1 implementation
static thread_local std::unique_ptr<Sha1Impl> tls_sha1_impl;

Sha1::Sha1() {
    reset();
}

void Sha1::reset() {
    if (!tls_sha1_impl) {
        tls_sha1_impl = std::make_unique<Sha1Impl>();
    } else {
        tls_sha1_impl->reset();
    }
}

void Sha1::update(const u8* data, u32 size) {
    if (tls_sha1_impl) {
        tls_sha1_impl->update(data, size);
    }
}

void Sha1::finalize(u8* hash) {
    if (tls_sha1_impl) {
        tls_sha1_impl->finalize(hash);
    }
}

void Sha1::hash(const u8* data, u32 size, u8* hash) {
    // Use mbedTLS one-shot SHA-1 function
    mbedtls_sha1(data, size, hash);
}

//=============================================================================
// XEX Decryptor
//=============================================================================

// Xbox 360 retail encryption key
const u8 XexDecryptor::retail_key_[16] = {
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91
};

// DevKit key (for development consoles)
const u8 XexDecryptor::devkit_key_[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// XEX1 key (pre-release format, used in early dev/beta builds)
const u8 XexDecryptor::xex1_key_[16] = {
    0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9,
    0x8B, 0x99, 0x92, 0x2C, 0xE9, 0x32, 0x15, 0x72
};

XexDecryptor::XexDecryptor() = default;
XexDecryptor::~XexDecryptor() = default;

void XexDecryptor::set_key(const u8* file_key, XexKeyType key_type) {
    u8 derived_key[16];
    derive_key(file_key, derived_key, key_type);
    aes_.set_key(derived_key);
    key_set_ = true;
    key_type_ = key_type;

    const char* key_name = "unknown";
    switch (key_type) {
        case XexKeyType::Retail: key_name = "retail"; break;
        case XexKeyType::DevKit: key_name = "devkit"; break;
        case XexKeyType::XEX1:   key_name = "xex1"; break;
    }
    LOGI("XEX encryption key set (type: %s)", key_name);
}

bool XexDecryptor::try_keys(u8* data, u32 size, const u8* file_key,
                             XexKeyType& out_key_type) {
    // Try each key type and check if decryption produces valid data
    // We test by decrypting a copy and checking for MZ/PE header
    static const XexKeyType key_order[] = {
        XexKeyType::Retail, XexKeyType::DevKit, XexKeyType::XEX1
    };

    for (auto kt : key_order) {
        std::vector<u8> test_data(data, data + std::min(size, 32u));

        u8 derived[16];
        derive_key(file_key, derived, kt);

        Aes128 test_aes;
        test_aes.set_key(derived);
        u8 iv[16] = {0};
        test_aes.decrypt_cbc(test_data.data(), static_cast<u32>(test_data.size()), iv);

        // Check for MZ header (DOS stub) as validation
        if (test_data.size() >= 2 && test_data[0] == 'M' && test_data[1] == 'Z') {
            out_key_type = kt;
            set_key(file_key, kt);
            return true;
        }
    }

    // Default to retail if no key produced valid header
    out_key_type = XexKeyType::Retail;
    set_key(file_key, XexKeyType::Retail);
    return false;
}

void XexDecryptor::derive_key(const u8* file_key, u8* derived_key, XexKeyType key_type) {
    const u8* master_key;
    switch (key_type) {
        case XexKeyType::DevKit: master_key = devkit_key_; break;
        case XexKeyType::XEX1:   master_key = xex1_key_; break;
        default:                 master_key = retail_key_; break;
    }

    Aes128 master_aes;
    master_aes.set_key(master_key);
    memcpy(derived_key, file_key, 16);
    master_aes.decrypt_ecb(derived_key, 16);
}

Status XexDecryptor::decrypt_header(u8* data, u32 size) {
    if (!key_set_) {
        LOGE("XEX key not set");
        return Status::Error;
    }
    
    aes_.decrypt_ecb(data, size);
    return Status::Ok;
}

Status XexDecryptor::decrypt_image(u8* data, u32 size, const u8* iv) {
    if (!key_set_) {
        LOGE("XEX key not set");
        return Status::Error;
    }

    aes_.decrypt_cbc(data, size, iv);
    return Status::Ok;
}

Status XexDecryptor::decompress_image(const u8* compressed, u32 comp_size,
                                       u8* decompressed, u32 decomp_size,
                                       XexCompression type) {
    switch (type) {
        case XexCompression::None:
            if (comp_size > decomp_size) return Status::Error;
            memcpy(decompressed, compressed, comp_size);
            return Status::Ok;
            
        case XexCompression::Basic:
            LOGE("Basic compression requires block info");
            return Status::Error;
            
        case XexCompression::Normal:
            return decompress_lzx(compressed, comp_size, decompressed, decomp_size);
            
        case XexCompression::Delta:
            LOGE("Delta compression requires base image data");
            return Status::Error;

        default:
            LOGE("Unknown compression type: %d", static_cast<int>(type));
            return Status::Error;
    }
}

Status XexDecryptor::decompress_delta(const u8* src, u32 src_size,
                                       const u8* base_data, u32 base_size,
                                       u8* dst, u32 dst_size) {
    // Delta compression applies patches to a base image
    // Format: series of (offset, size, data) tuples
    // Start with copy of base image
    if (base_size > dst_size) {
        LOGE("Base image larger than destination buffer");
        return Status::Error;
    }

    memcpy(dst, base_data, std::min(base_size, dst_size));

    // Parse delta patch blocks
    u32 src_pos = 0;
    while (src_pos + 8 <= src_size) {
        u32 patch_offset = (src[src_pos] << 24) | (src[src_pos+1] << 16) |
                           (src[src_pos+2] << 8) | src[src_pos+3];
        src_pos += 4;
        u32 patch_size = (src[src_pos] << 24) | (src[src_pos+1] << 16) |
                         (src[src_pos+2] << 8) | src[src_pos+3];
        src_pos += 4;

        if (patch_size == 0 && patch_offset == 0) break;  // End marker

        if (src_pos + patch_size > src_size || patch_offset + patch_size > dst_size) {
            LOGW("Delta patch out of bounds: offset=0x%X, size=0x%X", patch_offset, patch_size);
            break;
        }

        memcpy(dst + patch_offset, src + src_pos, patch_size);
        src_pos += patch_size;
    }

    LOGI("Applied delta patches: %u bytes of patch data", src_pos);
    return Status::Ok;
}

Status XexDecryptor::decompress_basic(const u8* src, u32 src_size,
                                       u8* dst, u32 dst_size,
                                       const CompressionBlock* blocks, u32 block_count) {
    u32 src_pos = 0;
    u32 dst_pos = 0;
    
    for (u32 i = 0; i < block_count && dst_pos < dst_size; i++) {
        u32 block_size = blocks[i].data_size;
        
        if (src_pos + block_size > src_size) {
            LOGE("Basic decompression overrun at block %u", i);
            return Status::Error;
        }
        
        if (blocks[i].hash[0] != 0 || blocks[i].hash[1] != 0) {
            if (!verify_hash(src + src_pos, block_size, blocks[i].hash)) {
                LOGE("Hash mismatch at block %u", i);
                return Status::Error;
            }
        }
        
        u32 copy_size = std::min(block_size, dst_size - dst_pos);
        memcpy(dst + dst_pos, src + src_pos, copy_size);
        
        src_pos += block_size;
        dst_pos += copy_size;
    }
    
    return Status::Ok;
}

Status XexDecryptor::decompress_lzx(const u8* src, u32 src_size,
                                     u8* dst, u32 dst_size,
                                     u32 window_size) {
    LzxDecompressor lzx;

    // Convert window size (bytes) to window bits (power of 2)
    // Find the highest bit set in window_size
    u32 window_bits = 15;  // Minimum LZX window size
    if (window_size > 0) {
        window_bits = 0;
        u32 temp = window_size;
        while (temp > 1) {
            temp >>= 1;
            window_bits++;
        }
        // Ensure we have enough bits (round up if not exact power of 2)
        if ((1u << window_bits) < window_size) {
            window_bits++;
        }
    }

    // Clamp to valid LZX range (15-21 bits)
    if (window_bits < 15) window_bits = 15;
    if (window_bits > 21) window_bits = 21;

    Status status = lzx.initialize(window_bits);
    if (status != Status::Ok) {
        return status;
    }

    return lzx.decompress(src, src_size, dst, dst_size);
}

bool XexDecryptor::verify_hash(const u8* data, u32 size, const u8* expected_hash) {
    u8 actual_hash[20];
    Sha1::hash(data, size, actual_hash);
    return memcmp(actual_hash, expected_hash, 20) == 0;
}

//=============================================================================
// LZX Decompressor using libmspack
//=============================================================================

// Memory-based mspack file structure for LZX decompression
namespace {

struct MemoryFile {
    const u8* data;
    u8* write_data;
    u32 size;
    u32 pos;
    bool is_output;
};

static struct mspack_file* mem_open(struct mspack_system* self, const char* filename, int mode) {
    (void)self; (void)filename; (void)mode;
    return nullptr;  // Not used - we create MemoryFile directly
}

static void mem_close(struct mspack_file* file) {
    (void)file;  // No-op - caller manages memory
}

static int mem_read(struct mspack_file* file, void* buffer, int bytes) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    if (!mf || mf->is_output) return -1;
    
    int avail = static_cast<int>(mf->size - mf->pos);
    int to_read = (bytes < avail) ? bytes : avail;
    if (to_read > 0) {
        memcpy(buffer, mf->data + mf->pos, static_cast<size_t>(to_read));
        mf->pos += static_cast<u32>(to_read);
    }
    return to_read;
}

static int mem_write(struct mspack_file* file, void* buffer, int bytes) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    if (!mf || !mf->is_output || !mf->write_data) return -1;
    
    // Check bounds
    if (mf->pos + static_cast<u32>(bytes) > mf->size) {
        bytes = static_cast<int>(mf->size - mf->pos);
    }
    if (bytes > 0) {
        memcpy(mf->write_data + mf->pos, buffer, static_cast<size_t>(bytes));
        mf->pos += static_cast<u32>(bytes);
    }
    return bytes;
}

static int mem_seek(struct mspack_file* file, off_t offset, int mode) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    if (!mf) return -1;
    
    switch (mode) {
        case MSPACK_SYS_SEEK_START:
            mf->pos = static_cast<u32>(offset);
            break;
        case MSPACK_SYS_SEEK_CUR:
            mf->pos = static_cast<u32>(static_cast<off_t>(mf->pos) + offset);
            break;
        case MSPACK_SYS_SEEK_END:
            mf->pos = static_cast<u32>(static_cast<off_t>(mf->size) + offset);
            break;
        default:
            return -1;
    }
    return 0;
}

static off_t mem_tell(struct mspack_file* file) {
    auto* mf = reinterpret_cast<MemoryFile*>(file);
    return mf ? static_cast<off_t>(mf->pos) : -1;
}

static void mem_msg(struct mspack_file* file, const char* format, ...) {
    (void)file; (void)format;
    // Messages could be logged here if needed
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
    mem_open,
    mem_close,
    mem_read,
    mem_write,
    mem_seek,
    mem_tell,
    mem_msg,
    mem_alloc,
    mem_free,
    mem_copy,
    nullptr
};

} // anonymous namespace

LzxDecompressor::LzxDecompressor() = default;
LzxDecompressor::~LzxDecompressor() = default;

Status LzxDecompressor::initialize(u32 window_bits) {
    if (window_bits < 15 || window_bits > 21) {
        LOGE("Invalid LZX window size: %u bits", window_bits);
        return Status::InvalidArgument;
    }
    
    window_bits_ = window_bits;
    window_size_ = 1u << window_bits;
    
    LOGD("LZX initialized: window_bits=%u, window_size=%u", window_bits_, window_size_);
    return Status::Ok;
}

void LzxDecompressor::reset() {
    // Reset is handled internally by libmspack
}

Status LzxDecompressor::decompress(const u8* src, u32 src_size,
                                    u8* dst, u32 dst_size) {
    // Create memory files for input and output
    MemoryFile input_file = { src, nullptr, src_size, 0, false };
    MemoryFile output_file = { nullptr, dst, dst_size, 0, true };
    
    // Initialize LZX decompressor
    // Match Xenia's parameters: window_bits, reset_interval=0, input_buffer=0x8000
    struct lzxd_stream* lzx = lzxd_init(
        &mem_system,
        reinterpret_cast<struct mspack_file*>(&input_file),
        reinterpret_cast<struct mspack_file*>(&output_file),
        static_cast<int>(window_bits_),
        0,              // reset_interval (0 = no reset, frame_size * 32768)
        0x8000,         // input buffer size (32KB, matching Xenia)
        static_cast<off_t>(dst_size),  // output length
        0               // is_delta (0 = normal LZX, not DELTA compression)
    );
    
    if (!lzx) {
        LOGE("Failed to initialize LZX decompressor");
        return Status::Error;
    }
    
    // Decompress
    int result = lzxd_decompress(lzx, static_cast<off_t>(dst_size));
    lzxd_free(lzx);
    
    if (result != MSPACK_ERR_OK) {
        LOGE("LZX decompression failed with error %d", result);
        return Status::Error;
    }
    
    LOGI("LZX decompression successful: %u -> %u bytes", src_size, dst_size);
    return Status::Ok;
}

} // namespace x360mu
