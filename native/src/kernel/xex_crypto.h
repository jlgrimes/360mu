/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX Cryptography
 * 
 * Handles decryption and decompression of Xbox 360 executables.
 * Retail XEX files are encrypted with AES-128-CBC and may be
 * compressed with LZX or basic block compression.
 */

#pragma once

#include "x360mu/types.h"
#include <vector>

namespace x360mu {

/**
 * AES-128 implementation using OpenSSL
 * Used for XEX decryption
 */
class Aes128 {
public:
    static constexpr u32 BLOCK_SIZE = 16;
    static constexpr u32 KEY_SIZE = 16;
    
    /**
     * Initialize with key
     */
    void set_key(const u8* key);
    
    /**
     * Decrypt a single block (in-place)
     */
    void decrypt_block(u8* data);
    
    /**
     * Decrypt CBC mode
     */
    void decrypt_cbc(u8* data, u32 size, const u8* iv);
    
    /**
     * Decrypt ECB mode
     */
    void decrypt_ecb(u8* data, u32 size);
};

/**
 * XEX encryption types
 */
enum class XexEncryption : u8 {
    None = 0,
    Normal = 1,    // Standard retail encryption
    DevKit = 2,    // Development kit (different key)
};

/**
 * XEX compression types
 */
enum class XexCompression : u8 {
    None = 0,
    Basic = 1,     // Basic block compression
    Normal = 2,    // LZX compression
    Delta = 3,     // Delta patch compression
};

/**
 * Compression block info
 */
struct CompressionBlock {
    u32 data_size;
    u8 hash[20];  // SHA-1
};

/**
 * XEX Decryptor
 * 
 * Handles all cryptographic operations for XEX files.
 */
class XexDecryptor {
public:
    XexDecryptor();
    ~XexDecryptor();
    
    /**
     * Set the encryption key from security info
     */
    void set_key(const u8* file_key);
    
    /**
     * Decrypt XEX header
     */
    Status decrypt_header(u8* data, u32 size);
    
    /**
     * Decrypt PE image data
     */
    Status decrypt_image(u8* data, u32 size, const u8* iv);
    
    /**
     * Decompress image data
     */
    Status decompress_image(const u8* compressed, u32 comp_size,
                            u8* decompressed, u32 decomp_size,
                            XexCompression type);
    
    /**
     * Decompress basic compression blocks
     */
    Status decompress_basic(const u8* src, u32 src_size,
                            u8* dst, u32 dst_size,
                            const CompressionBlock* blocks, u32 block_count);
    
    /**
     * Decompress LZX compression
     */
    Status decompress_lzx(const u8* src, u32 src_size,
                          u8* dst, u32 dst_size,
                          u32 window_size = 0x20000);
    
    /**
     * Verify SHA-1 hash
     */
    bool verify_hash(const u8* data, u32 size, const u8* expected_hash);
    
private:
    Aes128 aes_;
    bool key_set_ = false;
    
    // XEX keys
    static const u8 retail_key_[16];
    static const u8 devkit_key_[16];
    
    // Derive per-file key from master key
    void derive_key(const u8* file_key, u8* derived_key);
};

/**
 * LZX Decompressor
 * 
 * Microsoft's LZX compression used in Xbox 360 executables.
 * Uses libmspack for proper LZX decompression (BSD licensed).
 */
class LzxDecompressor {
public:
    LzxDecompressor();
    ~LzxDecompressor();
    
    /**
     * Initialize decompressor with window size
     * @param window_bits Window size as power of 2 (15-21)
     */
    Status initialize(u32 window_bits);
    
    /**
     * Reset state for new stream
     */
    void reset();
    
    /**
     * Decompress entire stream
     * @param src Source compressed data
     * @param src_size Size of compressed data
     * @param dst Destination buffer
     * @param dst_size Expected decompressed size
     */
    Status decompress(const u8* src, u32 src_size,
                      u8* dst, u32 dst_size);
    
private:
    u32 window_bits_ = 17;  // Default 128KB window
    u32 window_size_ = 0;
};

/**
 * SHA-1 implementation using OpenSSL
 * Used for XEX hash verification
 */
class Sha1 {
public:
    static constexpr u32 HASH_SIZE = 20;
    
    Sha1();
    
    void reset();
    void update(const u8* data, u32 size);
    void finalize(u8* hash);
    
    static void hash(const u8* data, u32 size, u8* hash);
};

} // namespace x360mu

