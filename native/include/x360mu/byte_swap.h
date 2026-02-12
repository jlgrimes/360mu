/**
 * 360mu - Xbox 360 Emulator for Android
 *
 * Byte-swap utilities for big-endian guest data
 * Xbox 360 (PowerPC) is big-endian, ARM64 Android is little-endian.
 *
 * Single-value swaps: use byte_swap<T>() from types.h
 * Bulk/array swaps: use functions in this header
 */

#pragma once

#include "x360mu/types.h"
#include <cstring>

namespace x360mu {

// ============================================================================
// Bulk array byte-swap (src -> dst)
// ============================================================================

/**
 * Byte-swap array of u16 values from src to dst
 * @param dst Destination (host-endian output)
 * @param src Source (big-endian guest data)
 * @param count Number of u16 elements
 */
inline void byte_swap_array_16(u16* dst, const u16* src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = byte_swap(src[i]);
    }
}

/**
 * Byte-swap array of u32 values from src to dst
 * @param dst Destination (host-endian output)
 * @param src Source (big-endian guest data)
 * @param count Number of u32 elements
 */
inline void byte_swap_array_32(u32* dst, const u32* src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = byte_swap(src[i]);
    }
}

/**
 * Byte-swap array of u64 values from src to dst
 * @param dst Destination (host-endian output)
 * @param src Source (big-endian guest data)
 * @param count Number of u64 elements
 */
inline void byte_swap_array_64(u64* dst, const u64* src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = byte_swap(src[i]);
    }
}

// ============================================================================
// In-place byte-swap
// ============================================================================

inline void byte_swap_in_place_16(u16* data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = byte_swap(data[i]);
    }
}

inline void byte_swap_in_place_32(u32* data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = byte_swap(data[i]);
    }
}

inline void byte_swap_in_place_64(u64* data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = byte_swap(data[i]);
    }
}

// ============================================================================
// Endian-aware copy with Xbox 360 swap modes
// ============================================================================

/**
 * Copy data with Xbox 360 endian swapping based on fetch constant mode
 * @param dst Destination buffer (host-endian)
 * @param src Source buffer (big-endian guest memory)
 * @param size Size in bytes
 * @param endian_swap Swap mode from fetch constant:
 *   0 = no swap
 *   1 = 8-in-16 (swap bytes within u16)
 *   2 = 8-in-32 (swap bytes within u32) — most common for Xbox 360
 *   3 = 16-in-32 (swap u16 halves within u32)
 */
inline void endian_copy(void* dst, const void* src, size_t size, u32 endian_swap) {
    switch (endian_swap) {
        case 0:
            memcpy(dst, src, size);
            break;

        case 1: {
            const u16* s = static_cast<const u16*>(src);
            u16* d = static_cast<u16*>(dst);
            size_t count = size / 2;
            byte_swap_array_16(d, s, count);
            if (size & 1) {
                static_cast<u8*>(dst)[size - 1] = static_cast<const u8*>(src)[size - 1];
            }
            break;
        }

        case 2: {
            const u32* s = static_cast<const u32*>(src);
            u32* d = static_cast<u32*>(dst);
            size_t count = size / 4;
            byte_swap_array_32(d, s, count);
            size_t remainder = size & 3;
            if (remainder) {
                memcpy(static_cast<u8*>(dst) + size - remainder,
                       static_cast<const u8*>(src) + size - remainder,
                       remainder);
            }
            break;
        }

        case 3: {
            const u32* s = static_cast<const u32*>(src);
            u32* d = static_cast<u32*>(dst);
            size_t count = size / 4;
            for (size_t i = 0; i < count; i++) {
                u32 v = s[i];
                d[i] = (v >> 16) | (v << 16);
            }
            size_t remainder = size & 3;
            if (remainder) {
                memcpy(static_cast<u8*>(dst) + size - remainder,
                       static_cast<const u8*>(src) + size - remainder,
                       remainder);
            }
            break;
        }

        default:
            memcpy(dst, src, size);
            break;
    }
}

} // namespace x360mu
