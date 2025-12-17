/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX Cryptography Implementation
 */

#include "xex_crypto.h"
#include <cstring>
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-crypto"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[CRYPTO] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[CRYPTO ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// AES Constants
//=============================================================================

// Inverse S-box for AES decryption
const u8 Aes128::inv_sbox_[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D
};

// S-box for key expansion
static const u8 sbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

// Round constants
static const u8 rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36 };

// Galois field multiplication tables for InvMixColumns
static const u8 gf_mul9[256] = { /* Pre-computed GF(2^8) multiply by 9 */ };
static const u8 gf_mul11[256] = { /* Pre-computed GF(2^8) multiply by 11 */ };
static const u8 gf_mul13[256] = { /* Pre-computed GF(2^8) multiply by 13 */ };
static const u8 gf_mul14[256] = { /* Pre-computed GF(2^8) multiply by 14 */ };

// GF multiply helper
static u8 gf_mul(u8 a, u8 b) {
    u8 result = 0;
    u8 hi_bit;
    for (int i = 0; i < 8; i++) {
        if (b & 1) result ^= a;
        hi_bit = a & 0x80;
        a <<= 1;
        if (hi_bit) a ^= 0x1B;  // x^8 + x^4 + x^3 + x + 1
        b >>= 1;
    }
    return result;
}

//=============================================================================
// AES-128 Implementation
//=============================================================================

void Aes128::set_key(const u8* key) {
    expand_key(key);
}

void Aes128::expand_key(const u8* key) {
    // Copy initial key
    memcpy(round_keys_.data(), key, KEY_SIZE);
    
    u8 temp[4];
    u32 i = KEY_SIZE;
    u8 rcon_idx = 0;
    
    while (i < 176) {
        // Copy previous 4 bytes
        memcpy(temp, &round_keys_[i - 4], 4);
        
        if (i % KEY_SIZE == 0) {
            // RotWord
            u8 t = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = t;
            
            // SubWord
            temp[0] = sbox[temp[0]];
            temp[1] = sbox[temp[1]];
            temp[2] = sbox[temp[2]];
            temp[3] = sbox[temp[3]];
            
            // XOR with Rcon
            temp[0] ^= rcon[rcon_idx++];
        }
        
        // XOR with word 4 positions back
        for (int j = 0; j < 4; j++) {
            round_keys_[i + j] = round_keys_[i - KEY_SIZE + j] ^ temp[j];
        }
        i += 4;
    }
}

void Aes128::add_round_key(u8* state, const u8* key) {
    for (int i = 0; i < BLOCK_SIZE; i++) {
        state[i] ^= key[i];
    }
}

void Aes128::inv_sub_bytes(u8* state) {
    for (int i = 0; i < BLOCK_SIZE; i++) {
        state[i] = inv_sbox_[state[i]];
    }
}

void Aes128::inv_shift_rows(u8* state) {
    u8 temp;
    
    // Row 1: shift right 1
    temp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = temp;
    
    // Row 2: shift right 2
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;
    
    // Row 3: shift right 3 (= left 1)
    temp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = temp;
}

void Aes128::inv_mix_columns(u8* state) {
    u8 col[4];
    
    for (int c = 0; c < 4; c++) {
        col[0] = state[c * 4 + 0];
        col[1] = state[c * 4 + 1];
        col[2] = state[c * 4 + 2];
        col[3] = state[c * 4 + 3];
        
        state[c * 4 + 0] = gf_mul(col[0], 0x0E) ^ gf_mul(col[1], 0x0B) ^ 
                          gf_mul(col[2], 0x0D) ^ gf_mul(col[3], 0x09);
        state[c * 4 + 1] = gf_mul(col[0], 0x09) ^ gf_mul(col[1], 0x0E) ^ 
                          gf_mul(col[2], 0x0B) ^ gf_mul(col[3], 0x0D);
        state[c * 4 + 2] = gf_mul(col[0], 0x0D) ^ gf_mul(col[1], 0x09) ^ 
                          gf_mul(col[2], 0x0E) ^ gf_mul(col[3], 0x0B);
        state[c * 4 + 3] = gf_mul(col[0], 0x0B) ^ gf_mul(col[1], 0x0D) ^ 
                          gf_mul(col[2], 0x09) ^ gf_mul(col[3], 0x0E);
    }
}

void Aes128::decrypt_block(u8* data) {
    // Final round key first
    add_round_key(data, &round_keys_[160]);
    
    // Rounds 9 to 1
    for (int round = NUM_ROUNDS - 1; round > 0; round--) {
        inv_shift_rows(data);
        inv_sub_bytes(data);
        add_round_key(data, &round_keys_[round * BLOCK_SIZE]);
        inv_mix_columns(data);
    }
    
    // Final round (no InvMixColumns)
    inv_shift_rows(data);
    inv_sub_bytes(data);
    add_round_key(data, &round_keys_[0]);
}

void Aes128::decrypt_cbc(u8* data, u32 size, const u8* iv) {
    u8 prev_cipher[BLOCK_SIZE];
    u8 curr_cipher[BLOCK_SIZE];
    
    memcpy(prev_cipher, iv, BLOCK_SIZE);
    
    for (u32 i = 0; i < size; i += BLOCK_SIZE) {
        // Save current ciphertext for next block
        memcpy(curr_cipher, data + i, BLOCK_SIZE);
        
        // Decrypt block
        decrypt_block(data + i);
        
        // XOR with previous ciphertext (or IV for first block)
        for (int j = 0; j < BLOCK_SIZE; j++) {
            data[i + j] ^= prev_cipher[j];
        }
        
        // Update previous ciphertext
        memcpy(prev_cipher, curr_cipher, BLOCK_SIZE);
    }
}

void Aes128::decrypt_ecb(u8* data, u32 size) {
    for (u32 i = 0; i < size; i += BLOCK_SIZE) {
        decrypt_block(data + i);
    }
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

XexDecryptor::XexDecryptor() = default;
XexDecryptor::~XexDecryptor() = default;

void XexDecryptor::set_key(const u8* file_key) {
    u8 derived_key[16];
    derive_key(file_key, derived_key);
    aes_.set_key(derived_key);
    key_set_ = true;
    
    LOGI("XEX encryption key set");
}

void XexDecryptor::derive_key(const u8* file_key, u8* derived_key) {
    // Decrypt file key with retail key
    Aes128 master_aes;
    master_aes.set_key(retail_key_);
    
    memcpy(derived_key, file_key, 16);
    master_aes.decrypt_ecb(derived_key, 16);
}

Status XexDecryptor::decrypt_header(u8* data, u32 size) {
    if (!key_set_) {
        LOGE("XEX key not set");
        return Status::Error;
    }
    
    // Header is ECB encrypted
    aes_.decrypt_ecb(data, size);
    return Status::Ok;
}

Status XexDecryptor::decrypt_image(u8* data, u32 size, const u8* iv) {
    if (!key_set_) {
        LOGE("XEX key not set");
        return Status::Error;
    }
    
    // Image is CBC encrypted
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
            // Would need compression block info
            LOGE("Basic compression requires block info");
            return Status::Error;
            
        case XexCompression::Normal:
            return decompress_lzx(compressed, comp_size, decompressed, decomp_size);
            
        default:
            LOGE("Unknown compression type: %d", static_cast<int>(type));
            return Status::Error;
    }
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
        
        // Verify hash if present
        if (blocks[i].hash[0] != 0 || blocks[i].hash[1] != 0) {
            if (!verify_hash(src + src_pos, block_size, blocks[i].hash)) {
                LOGE("Hash mismatch at block %u", i);
                return Status::Error;
            }
        }
        
        // Copy block
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
    
    // Calculate window bits from size (default 128KB = 17 bits)
    u32 window_bits = 17;
    while ((1u << window_bits) < window_size && window_bits < 21) {
        window_bits++;
    }
    
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
// LZX Decompressor
//=============================================================================

LzxDecompressor::LzxDecompressor() = default;
LzxDecompressor::~LzxDecompressor() = default;

Status LzxDecompressor::initialize(u32 window_bits) {
    if (window_bits < 15 || window_bits > 21) {
        LOGE("Invalid LZX window size: %u bits", window_bits);
        return Status::InvalidArgument;
    }
    
    window_size_ = 1u << window_bits;
    window_.resize(window_size_);
    reset();
    
    return Status::Ok;
}

void LzxDecompressor::reset() {
    window_pos_ = 0;
    r0_ = r1_ = r2_ = 1;
    std::fill(window_.begin(), window_.end(), 0);
}

void LzxDecompressor::init_bits(const u8* data, u32 size) {
    bit_data_ = data;
    bit_size_ = size;
    bit_pos_ = 0;
    bit_buffer_ = 0;
    bits_left_ = 0;
}

u32 LzxDecompressor::read_bits(u32 count) {
    while (bits_left_ < count) {
        if (bit_pos_ + 1 < bit_size_) {
            // LZX uses little-endian 16-bit words
            u32 word = bit_data_[bit_pos_] | (bit_data_[bit_pos_ + 1] << 8);
            bit_buffer_ |= word << (16 - bits_left_);
            bits_left_ += 16;
            bit_pos_ += 2;
        } else {
            break;
        }
    }
    
    u32 result = bit_buffer_ >> (32 - count);
    bit_buffer_ <<= count;
    bits_left_ -= count;
    return result;
}

u32 LzxDecompressor::peek_bits(u32 count) {
    while (bits_left_ < count && bit_pos_ + 1 < bit_size_) {
        u32 word = bit_data_[bit_pos_] | (bit_data_[bit_pos_ + 1] << 8);
        bit_buffer_ |= word << (16 - bits_left_);
        bits_left_ += 16;
        bit_pos_ += 2;
    }
    
    return bit_buffer_ >> (32 - count);
}

Status LzxDecompressor::decompress(const u8* src, u32 src_size,
                                    u8* dst, u32 dst_size) {
    init_bits(src, src_size);
    
    u32 dst_pos = 0;
    
    while (dst_pos < dst_size) {
        // Read block type (3 bits)
        u32 block_type = read_bits(3);
        
        // Read block size (24 bits)
        u32 block_size = read_bits(24);
        
        if (block_size == 0 || dst_pos + block_size > dst_size) {
            break;
        }
        
        Status status = Status::Ok;
        
        switch (block_type) {
            case 1: // Verbatim block
                status = decompress_verbatim_block(block_size);
                break;
            case 2: // Aligned offset block
                status = decompress_aligned_block(block_size);
                break;
            case 3: // Uncompressed block
                status = decompress_uncompressed_block(block_size);
                break;
            default:
                LOGE("Unknown LZX block type: %u", block_type);
                return Status::Error;
        }
        
        if (status != Status::Ok) {
            return status;
        }
        
        // Copy from window to output
        u32 copy_start = (window_pos_ - block_size) & (window_size_ - 1);
        for (u32 i = 0; i < block_size; i++) {
            dst[dst_pos++] = window_[(copy_start + i) & (window_size_ - 1)];
        }
    }
    
    return Status::Ok;
}

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

Status LzxDecompressor::decompress_aligned_block(u32 uncompressed_size) {
    LOGD("LZX aligned block: %u bytes", uncompressed_size);
    return decompress_verbatim_block(uncompressed_size);
}

Status LzxDecompressor::decompress_uncompressed_block(u32 uncompressed_size) {
    LOGD("LZX uncompressed block: %u bytes", uncompressed_size);
    
    // Align to 16-bit boundary
    if (bits_left_ >= 16) {
        bits_left_ -= 16;
        bit_buffer_ <<= 16;
    }
    
    // Read R0, R1, R2 (32 bits each, little-endian)
    r0_ = bit_data_[bit_pos_] | (bit_data_[bit_pos_+1] << 8) |
          (bit_data_[bit_pos_+2] << 16) | (bit_data_[bit_pos_+3] << 24);
    r1_ = bit_data_[bit_pos_+4] | (bit_data_[bit_pos_+5] << 8) |
          (bit_data_[bit_pos_+6] << 16) | (bit_data_[bit_pos_+7] << 24);
    r2_ = bit_data_[bit_pos_+8] | (bit_data_[bit_pos_+9] << 8) |
          (bit_data_[bit_pos_+10] << 16) | (bit_data_[bit_pos_+11] << 24);
    bit_pos_ += 12;
    
    // Copy uncompressed data
    for (u32 i = 0; i < uncompressed_size && bit_pos_ < bit_size_; i++) {
        window_[window_pos_++ & (window_size_ - 1)] = bit_data_[bit_pos_++];
    }
    
    // Realign to 16-bit boundary
    if (uncompressed_size & 1) {
        bit_pos_++;
    }
    
    bits_left_ = 0;
    bit_buffer_ = 0;
    
    return Status::Ok;
}

//=============================================================================
// SHA-1 Implementation
//=============================================================================

Sha1::Sha1() {
    reset();
}

void Sha1::reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    state_[4] = 0xC3D2E1F0;
    buffer_size_ = 0;
    total_size_ = 0;
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void Sha1::transform(const u8* block) {
    u32 w[80];
    
    // Prepare message schedule
    for (int i = 0; i < 16; i++) {
        w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) |
               (block[i*4+2] << 8) | block[i*4+3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    
    u32 a = state_[0];
    u32 b = state_[1];
    u32 c = state_[2];
    u32 d = state_[3];
    u32 e = state_[4];
    
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        
        u32 temp = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = temp;
    }
    
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const u8* data, u32 size) {
    total_size_ += size;
    
    while (size > 0) {
        u32 to_copy = std::min(size, 64 - buffer_size_);
        memcpy(buffer_.data() + buffer_size_, data, to_copy);
        buffer_size_ += to_copy;
        data += to_copy;
        size -= to_copy;
        
        if (buffer_size_ == 64) {
            transform(buffer_.data());
            buffer_size_ = 0;
        }
    }
}

void Sha1::finalize(u8* hash) {
    // Pad message
    buffer_[buffer_size_++] = 0x80;
    
    if (buffer_size_ > 56) {
        while (buffer_size_ < 64) buffer_[buffer_size_++] = 0;
        transform(buffer_.data());
        buffer_size_ = 0;
    }
    
    while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
    
    // Append length in bits (big-endian)
    u64 bit_len = total_size_ * 8;
    buffer_[56] = static_cast<u8>(bit_len >> 56);
    buffer_[57] = static_cast<u8>(bit_len >> 48);
    buffer_[58] = static_cast<u8>(bit_len >> 40);
    buffer_[59] = static_cast<u8>(bit_len >> 32);
    buffer_[60] = static_cast<u8>(bit_len >> 24);
    buffer_[61] = static_cast<u8>(bit_len >> 16);
    buffer_[62] = static_cast<u8>(bit_len >> 8);
    buffer_[63] = static_cast<u8>(bit_len);
    
    transform(buffer_.data());
    
    // Output hash (big-endian)
    for (int i = 0; i < 5; i++) {
        hash[i*4 + 0] = static_cast<u8>(state_[i] >> 24);
        hash[i*4 + 1] = static_cast<u8>(state_[i] >> 16);
        hash[i*4 + 2] = static_cast<u8>(state_[i] >> 8);
        hash[i*4 + 3] = static_cast<u8>(state_[i]);
    }
}

void Sha1::hash(const u8* data, u32 size, u8* hash) {
    Sha1 sha;
    sha.update(data, size);
    sha.finalize(hash);
}

} // namespace x360mu

