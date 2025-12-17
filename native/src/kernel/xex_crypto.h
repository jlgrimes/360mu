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
#include <array>

namespace x360mu {

/**
 * AES-128 implementation
 * Used for XEX decryption
 */
class Aes128 {
public:
    static constexpr u32 BLOCK_SIZE = 16;
    static constexpr u32 KEY_SIZE = 16;
    static constexpr u32 NUM_ROUNDS = 10;
    
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
    
private:
    // Expanded key schedule (11 round keys)
    std::array<u8, 176> round_keys_;
    
    // Key expansion
    void expand_key(const u8* key);
    
    // AES round operations
    void inv_sub_bytes(u8* state);
    void inv_shift_rows(u8* state);
    void inv_mix_columns(u8* state);
    void add_round_key(u8* state, const u8* key);
    
    // S-box lookup
    static const u8 inv_sbox_[256];
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
 * Based on cabinet format LZX but with some modifications.
 */
class LzxDecompressor {
public:
    LzxDecompressor();
    ~LzxDecompressor();
    
    /**
     * Initialize decompressor
     */
    Status initialize(u32 window_bits);
    
    /**
     * Reset state for new stream
     */
    void reset();
    
    /**
     * Decompress a block
     */
    Status decompress_block(const u8* src, u32 src_size,
                            u8* dst, u32 dst_size,
                            u32& bytes_read, u32& bytes_written);
    
    /**
     * Decompress entire stream
     */
    Status decompress(const u8* src, u32 src_size,
                      u8* dst, u32 dst_size);
    
private:
    // Window
    std::vector<u8> window_;
    u32 window_size_ = 0;
    u32 window_pos_ = 0;
    
    // LZX state
    u32 r0_ = 1, r1_ = 1, r2_ = 1;  // Recent match offsets
    
    // Huffman tables
    struct HuffmanTable {
        std::vector<u16> lengths;
        std::vector<u16> codes;
        u32 max_bits;
    };
    
    HuffmanTable main_table_;
    HuffmanTable length_table_;
    HuffmanTable aligned_table_;
    
    // Bit reader
    const u8* bit_data_;
    u32 bit_size_;
    u32 bit_pos_;
    u32 bit_buffer_;
    u32 bits_left_;
    
    u32 read_bits(u32 count);
    u32 peek_bits(u32 count);
    void init_bits(const u8* data, u32 size);
    
    // Huffman decoding
    Status build_huffman_table(HuffmanTable& table, const u8* lengths, u32 count);
    u32 decode_huffman(const HuffmanTable& table);
    
    // Block decompression
    Status decompress_verbatim_block(u32 uncompressed_size);
    Status decompress_aligned_block(u32 uncompressed_size);
    Status decompress_uncompressed_block(u32 uncompressed_size);
};

/**
 * SHA-1 implementation
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
    
private:
    std::array<u32, 5> state_;
    std::array<u8, 64> buffer_;
    u32 buffer_size_;
    u64 total_size_;
    
    void transform(const u8* block);
};

} // namespace x360mu

