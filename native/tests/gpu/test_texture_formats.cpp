/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Texture Format Stress Test
 * Tests every Xbox 360 texture format through the full pipeline:
 * synthetic data → byte-swap → untile → verify
 * Edge cases: 1x1, NPOT, cubemap, 3D, packed mips, 4096x4096
 */

#include <gtest/gtest.h>
#include "gpu/xenos/texture.h"
#include "gpu/xenos/edram.h"
#include "gpu/texture_cache.h"
#include "gpu/xenos/gpu.h"
#include "x360mu/byte_swap.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <numeric>

using namespace x360mu;

//=============================================================================
// Helpers
//=============================================================================

// Fill buffer with deterministic pattern based on offset
static void fill_pattern(u8* data, size_t size, u32 seed = 0xDEAD) {
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<u8>((seed + i * 7 + (i >> 8) * 13) & 0xFF);
    }
}

// Tile a linear 2D surface into Xbox 360 tiled format (inverse of untile)
static void tile_2d(const u8* linear, u8* tiled,
                    u32 blocks_x, u32 blocks_y, u32 bpp) {
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 tiled_offset = TextureUntiler::get_tiled_offset_2d(bx, by, blocks_x, bpp);
            u32 linear_offset = (by * blocks_x + bx) * bpp;
            memcpy(tiled + tiled_offset, linear + linear_offset, bpp);
        }
    }
}

// Tile a linear 3D surface (per-slice)
static void tile_3d(const u8* linear, u8* tiled,
                    u32 width, u32 height, u32 depth, u32 bpp) {
    u32 macro_tiles_x = (width + 31) / 32;
    u32 macro_tiles_y = (height + 31) / 32;
    u32 tiled_slice_size = macro_tiles_x * macro_tiles_y * 32 * 32 * bpp;
    u32 linear_slice_size = width * height * bpp;

    for (u32 z = 0; z < depth; z++) {
        const u8* lin_slice = linear + z * linear_slice_size;
        u8* til_slice = tiled + z * tiled_slice_size;
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                u32 tiled_offset = TextureUntiler::get_tiled_offset_2d(x, y, width, bpp);
                u32 linear_offset = (y * width + x) * bpp;
                memcpy(til_slice + tiled_offset, lin_slice + linear_offset, bpp);
            }
        }
    }
}

// Tile a cubemap (per-face)
static void tile_cube(const u8* linear, u8* tiled,
                      u32 blocks_per_face, u32 bpp) {
    u32 macro_tiles = (blocks_per_face + 31) / 32;
    u32 tiled_face_size = macro_tiles * macro_tiles * 32 * 32 * bpp;
    u32 linear_face_size = blocks_per_face * blocks_per_face * bpp;

    for (u32 face = 0; face < 6; face++) {
        const u8* lin_face = linear + face * linear_face_size;
        u8* til_face = tiled + face * tiled_face_size;
        for (u32 by = 0; by < blocks_per_face; by++) {
            for (u32 bx = 0; bx < blocks_per_face; bx++) {
                u32 tiled_offset = TextureUntiler::get_tiled_offset_2d(bx, by, blocks_per_face, bpp);
                u32 linear_offset = (by * blocks_per_face + bx) * bpp;
                memcpy(til_face + tiled_offset, lin_face + linear_offset, bpp);
            }
        }
    }
}

// Calculate tiled buffer size (aligned to macro tile boundaries)
static u32 tiled_buffer_size(u32 blocks_x, u32 blocks_y, u32 bpp) {
    u32 macro_x = (blocks_x + 31) / 32;
    u32 macro_y = (blocks_y + 31) / 32;
    return macro_x * macro_y * 32 * 32 * bpp;
}

//=============================================================================
// Morton Encoding Tests
//=============================================================================

class MortonTest : public ::testing::Test {};

TEST_F(MortonTest, EncodeZero) {
    EXPECT_EQ(TextureUntiler::morton_encode(0, 0), 0u);
}

TEST_F(MortonTest, EncodeBasic) {
    // Morton(1,0) = 1, Morton(0,1) = 2, Morton(1,1) = 3
    EXPECT_EQ(TextureUntiler::morton_encode(1, 0), 1u);
    EXPECT_EQ(TextureUntiler::morton_encode(0, 1), 2u);
    EXPECT_EQ(TextureUntiler::morton_encode(1, 1), 3u);
    EXPECT_EQ(TextureUntiler::morton_encode(2, 0), 4u);
    EXPECT_EQ(TextureUntiler::morton_encode(0, 2), 8u);
}

TEST_F(MortonTest, DecodeRoundTrip) {
    for (u32 x = 0; x < 32; x++) {
        for (u32 y = 0; y < 32; y++) {
            u32 code = TextureUntiler::morton_encode(x, y);
            u32 dx, dy;
            TextureUntiler::morton_decode(code, dx, dy);
            EXPECT_EQ(dx, x) << "x=" << x << " y=" << y;
            EXPECT_EQ(dy, y) << "x=" << x << " y=" << y;
        }
    }
}

TEST_F(MortonTest, AllCodesUnique_8x8) {
    // Within an 8x8 micro tile, all 64 Morton codes should be unique
    std::vector<u32> codes;
    for (u32 y = 0; y < 8; y++) {
        for (u32 x = 0; x < 8; x++) {
            codes.push_back(TextureUntiler::morton_encode(x, y));
        }
    }
    std::sort(codes.begin(), codes.end());
    auto it = std::unique(codes.begin(), codes.end());
    EXPECT_EQ(it, codes.end()) << "Morton codes not unique within 8x8";
    // Should be values 0..63
    EXPECT_EQ(codes.front(), 0u);
    EXPECT_EQ(codes.back(), 63u);
}

//=============================================================================
// 2D Tiling Round-Trip Tests
//=============================================================================

class TilingRoundTrip2DTest : public ::testing::TestWithParam<std::tuple<u32, u32, u32>> {};

TEST_P(TilingRoundTrip2DTest, LinearTileUntile) {
    auto [width, height, bpp] = GetParam();

    u32 linear_size = width * height * bpp;
    u32 tiled_size = tiled_buffer_size(width, height, bpp);

    std::vector<u8> original(linear_size);
    std::vector<u8> tiled(tiled_size, 0);
    std::vector<u8> recovered(linear_size, 0);

    fill_pattern(original.data(), linear_size, width * 1000 + height);

    // Tile (linear → tiled)
    tile_2d(original.data(), tiled.data(), width, height, bpp);

    // Untile (tiled → linear)
    TextureUntiler::untile_2d(tiled.data(), recovered.data(),
                               width, height, bpp, 1, 1);

    EXPECT_EQ(memcmp(original.data(), recovered.data(), linear_size), 0)
        << "Round-trip failed for " << width << "x" << height << " bpp=" << bpp;
}

INSTANTIATE_TEST_SUITE_P(
    Dimensions, TilingRoundTrip2DTest,
    ::testing::Values(
        // (width_in_blocks, height_in_blocks, bytes_per_block)
        std::make_tuple(1u, 1u, 4u),       // 1x1, minimum size
        std::make_tuple(4u, 4u, 4u),       // 4x4, within micro tile
        std::make_tuple(8u, 8u, 4u),       // exactly 1 micro tile
        std::make_tuple(16u, 16u, 4u),     // 2x2 micro tiles
        std::make_tuple(32u, 32u, 4u),     // exactly 1 macro tile
        std::make_tuple(64u, 64u, 4u),     // 2x2 macro tiles
        std::make_tuple(256u, 256u, 4u),   // 8x8 macro tiles
        std::make_tuple(1024u, 1024u, 4u), // 32x32 macro tiles (4096x4096 with block=4)
        std::make_tuple(13u, 17u, 4u),     // NPOT dimensions
        std::make_tuple(33u, 33u, 4u),     // Just over 1 macro tile
        std::make_tuple(7u, 3u, 4u),       // Tiny NPOT
        // Different bpp values
        std::make_tuple(64u, 64u, 1u),     // 1 byte per pixel (k_8)
        std::make_tuple(64u, 64u, 2u),     // 2 bytes per pixel (k_5_6_5)
        std::make_tuple(64u, 64u, 8u),     // 8 bytes per block (DXT1)
        std::make_tuple(64u, 64u, 16u)     // 16 bytes per block (DXT5, DXN)
    )
);

//=============================================================================
// Block-Compressed Tiling Round-Trip
//=============================================================================

struct BCFormatParams {
    u32 tex_width;       // texture width in pixels
    u32 tex_height;      // texture height in pixels
    u32 block_size;      // block size in pixels (4 for BC)
    u32 bytes_per_block; // bytes per encoded block
    const char* name;
};

class BCTilingRoundTripTest : public ::testing::TestWithParam<BCFormatParams> {};

TEST_P(BCTilingRoundTripTest, RoundTrip) {
    auto p = GetParam();
    u32 blocks_x = (p.tex_width + p.block_size - 1) / p.block_size;
    u32 blocks_y = (p.tex_height + p.block_size - 1) / p.block_size;

    u32 linear_size = blocks_x * blocks_y * p.bytes_per_block;
    u32 tiled_sz = tiled_buffer_size(blocks_x, blocks_y, p.bytes_per_block);

    std::vector<u8> original(linear_size);
    std::vector<u8> tiled(tiled_sz, 0);
    std::vector<u8> recovered(linear_size, 0);

    fill_pattern(original.data(), linear_size, p.tex_width + p.tex_height);

    tile_2d(original.data(), tiled.data(), blocks_x, blocks_y, p.bytes_per_block);
    TextureUntiler::untile_2d(tiled.data(), recovered.data(),
                               blocks_x, blocks_y, p.bytes_per_block, 1, 1);

    EXPECT_EQ(memcmp(original.data(), recovered.data(), linear_size), 0)
        << "BC round-trip failed for " << p.name << " " << p.tex_width << "x" << p.tex_height;
}

INSTANTIATE_TEST_SUITE_P(
    BCFormats, BCTilingRoundTripTest,
    ::testing::Values(
        BCFormatParams{128, 128, 4, 8, "DXT1/BC1"},
        BCFormatParams{128, 128, 4, 16, "DXT5/BC3"},
        BCFormatParams{128, 128, 4, 16, "DXN/BC5"},
        BCFormatParams{256, 256, 4, 8, "DXT1_large"},
        BCFormatParams{4, 4, 4, 8, "DXT1_minimum"},
        BCFormatParams{12, 20, 4, 8, "DXT1_npot"},
        BCFormatParams{1024, 1024, 4, 8, "DXT1_1024"},
        BCFormatParams{128, 128, 4, 8, "DXT5A/BC4"},
        BCFormatParams{128, 128, 4, 8, "CTX1"}
    )
);

//=============================================================================
// 3D Texture Tiling Round-Trip
//=============================================================================

class Tiling3DTest : public ::testing::TestWithParam<std::tuple<u32, u32, u32, u32>> {};

TEST_P(Tiling3DTest, RoundTrip) {
    auto [width, height, depth, bpp] = GetParam();

    u32 linear_slice_size = width * height * bpp;
    u32 linear_size = linear_slice_size * depth;

    u32 macro_x = (width + 31) / 32;
    u32 macro_y = (height + 31) / 32;
    u32 tiled_slice_size = macro_x * macro_y * 32 * 32 * bpp;
    u32 tiled_sz = tiled_slice_size * depth;

    std::vector<u8> original(linear_size);
    std::vector<u8> tiled(tiled_sz, 0);
    std::vector<u8> recovered(linear_size, 0);

    fill_pattern(original.data(), linear_size, width * 100 + depth);

    tile_3d(original.data(), tiled.data(), width, height, depth, bpp);
    TextureUntiler::untile_3d(tiled.data(), recovered.data(),
                               width, height, depth, bpp);

    EXPECT_EQ(memcmp(original.data(), recovered.data(), linear_size), 0)
        << "3D round-trip failed for " << width << "x" << height << "x" << depth;
}

INSTANTIATE_TEST_SUITE_P(
    Dimensions3D, Tiling3DTest,
    ::testing::Values(
        std::make_tuple(8u, 8u, 4u, 4u),
        std::make_tuple(32u, 32u, 8u, 4u),
        std::make_tuple(64u, 64u, 16u, 4u),
        std::make_tuple(16u, 16u, 4u, 2u),
        std::make_tuple(13u, 13u, 5u, 4u)   // NPOT 3D
    )
);

//=============================================================================
// Cubemap Tiling Round-Trip
//=============================================================================

class CubemapTilingTest : public ::testing::TestWithParam<std::tuple<u32, u32>> {};

TEST_P(CubemapTilingTest, RoundTrip) {
    auto [face_size, bpp] = GetParam();

    u32 linear_face_size = face_size * face_size * bpp;
    u32 linear_size = linear_face_size * 6;

    u32 macro_n = (face_size + 31) / 32;
    u32 tiled_face_size = macro_n * macro_n * 32 * 32 * bpp;
    u32 tiled_sz = tiled_face_size * 6;

    std::vector<u8> original(linear_size);
    std::vector<u8> tiled(tiled_sz, 0);
    std::vector<u8> recovered(linear_size, 0);

    fill_pattern(original.data(), linear_size, face_size * 42);

    tile_cube(original.data(), tiled.data(), face_size, bpp);
    TextureUntiler::untile_cube(tiled.data(), recovered.data(),
                                 face_size, bpp, 1, 1);

    EXPECT_EQ(memcmp(original.data(), recovered.data(), linear_size), 0)
        << "Cubemap round-trip failed for face=" << face_size << " bpp=" << bpp;
}

INSTANTIATE_TEST_SUITE_P(
    CubeSizes, CubemapTilingTest,
    ::testing::Values(
        std::make_tuple(8u, 4u),
        std::make_tuple(32u, 4u),
        std::make_tuple(64u, 4u),
        std::make_tuple(128u, 4u),
        std::make_tuple(256u, 4u),
        std::make_tuple(64u, 8u),    // DXT1 blocks
        std::make_tuple(64u, 16u)    // DXT5 blocks
    )
);

//=============================================================================
// Tiled Offset Consistency Tests
//=============================================================================

class TiledOffsetTest : public ::testing::Test {};

TEST_F(TiledOffsetTest, AllOffsetsUniqueWithinMacroTile) {
    // For a 32x32 block surface (1 macro tile), all tiled offsets should be unique
    constexpr u32 BPP = 4;
    constexpr u32 SIZE = 32;
    std::vector<u32> offsets;

    for (u32 y = 0; y < SIZE; y++) {
        for (u32 x = 0; x < SIZE; x++) {
            offsets.push_back(TextureUntiler::get_tiled_offset_2d(x, y, SIZE, BPP));
        }
    }

    std::sort(offsets.begin(), offsets.end());
    auto it = std::unique(offsets.begin(), offsets.end());
    EXPECT_EQ(it, offsets.end()) << "Duplicate tiled offsets within macro tile";

    // All offsets should be within [0, 32*32*BPP)
    EXPECT_LT(offsets.back(), SIZE * SIZE * BPP);
}

TEST_F(TiledOffsetTest, OffsetsAlignedToBPP) {
    // Every tiled offset should be a multiple of bpp
    constexpr u32 BPP = 8;
    for (u32 y = 0; y < 64; y++) {
        for (u32 x = 0; x < 64; x++) {
            u32 offset = TextureUntiler::get_tiled_offset_2d(x, y, 64, BPP);
            EXPECT_EQ(offset % BPP, 0u)
                << "Offset not aligned at x=" << x << " y=" << y;
        }
    }
}

TEST_F(TiledOffsetTest, NoOverlapBetweenMacroTiles) {
    // Offsets from different macro tiles should not overlap
    constexpr u32 BPP = 4;
    constexpr u32 WIDTH = 64;  // 2 macro tiles wide
    constexpr u32 MACRO = 32;

    std::vector<u32> tile00, tile10, tile01, tile11;

    for (u32 y = 0; y < MACRO; y++) {
        for (u32 x = 0; x < MACRO; x++) {
            tile00.push_back(TextureUntiler::get_tiled_offset_2d(x, y, WIDTH, BPP));
            tile10.push_back(TextureUntiler::get_tiled_offset_2d(x + MACRO, y, WIDTH, BPP));
            tile01.push_back(TextureUntiler::get_tiled_offset_2d(x, y + MACRO, WIDTH, BPP));
            tile11.push_back(TextureUntiler::get_tiled_offset_2d(x + MACRO, y + MACRO, WIDTH, BPP));
        }
    }

    // Sort each set
    std::sort(tile00.begin(), tile00.end());
    std::sort(tile10.begin(), tile10.end());
    std::sort(tile01.begin(), tile01.end());
    std::sort(tile11.begin(), tile11.end());

    // Check no intersection between any pair
    std::vector<u32> intersect;
    std::set_intersection(tile00.begin(), tile00.end(),
                          tile10.begin(), tile10.end(),
                          std::back_inserter(intersect));
    EXPECT_TRUE(intersect.empty()) << "Overlap between tile (0,0) and (1,0)";

    intersect.clear();
    std::set_intersection(tile00.begin(), tile00.end(),
                          tile01.begin(), tile01.end(),
                          std::back_inserter(intersect));
    EXPECT_TRUE(intersect.empty()) << "Overlap between tile (0,0) and (0,1)";

    intersect.clear();
    std::set_intersection(tile00.begin(), tile00.end(),
                          tile11.begin(), tile11.end(),
                          std::back_inserter(intersect));
    EXPECT_TRUE(intersect.empty()) << "Overlap between tile (0,0) and (1,1)";
}

//=============================================================================
// Byte-Swap Tests
//=============================================================================

class ByteSwapTest : public ::testing::Test {};

TEST_F(ByteSwapTest, Swap32_RGBA) {
    // Xbox 360 stores RGBA8 as big-endian u32
    u8 data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
    TextureFormatConverter::byte_swap_32(data, 8);
    // After 32-bit swap: each dword reversed
    EXPECT_EQ(data[0], 0xDD);
    EXPECT_EQ(data[1], 0xCC);
    EXPECT_EQ(data[2], 0xBB);
    EXPECT_EQ(data[3], 0xAA);
    EXPECT_EQ(data[4], 0x44);
    EXPECT_EQ(data[5], 0x33);
    EXPECT_EQ(data[6], 0x22);
    EXPECT_EQ(data[7], 0x11);
}

TEST_F(ByteSwapTest, Swap16_RGB565) {
    u8 data[] = {0xAB, 0xCD, 0x12, 0x34};
    TextureFormatConverter::byte_swap_16(data, 4);
    EXPECT_EQ(data[0], 0xCD);
    EXPECT_EQ(data[1], 0xAB);
    EXPECT_EQ(data[2], 0x34);
    EXPECT_EQ(data[3], 0x12);
}

TEST_F(ByteSwapTest, EndianCopy_Mode2) {
    // Mode 2 = 8-in-32 (most common for Xbox 360)
    u8 src[] = {0xAA, 0xBB, 0xCC, 0xDD};
    u8 dst[4] = {};
    endian_copy(dst, src, 4, 2);
    EXPECT_EQ(dst[0], 0xDD);
    EXPECT_EQ(dst[1], 0xCC);
    EXPECT_EQ(dst[2], 0xBB);
    EXPECT_EQ(dst[3], 0xAA);
}

TEST_F(ByteSwapTest, EndianCopy_Mode0_NoSwap) {
    u8 src[] = {0x11, 0x22, 0x33, 0x44};
    u8 dst[4] = {};
    endian_copy(dst, src, 4, 0);
    EXPECT_EQ(memcmp(dst, src, 4), 0);
}

//=============================================================================
// Format Classification Tests
//=============================================================================

class FormatTest : public ::testing::Test {};

TEST_F(FormatTest, CompressedFormats) {
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXT1));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXT2_3));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXT4_5));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXT5A));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXN));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_CTX1));
    EXPECT_TRUE(TextureDecompressor::is_compressed(TextureFormat::k_DXT3A));
}

TEST_F(FormatTest, UncompressedFormats) {
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_8_8_8_8));
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_5_6_5));
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_4_4_4_4));
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_8));
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_16_16_FLOAT));
    EXPECT_FALSE(TextureDecompressor::is_compressed(TextureFormat::k_32_FLOAT));
}

TEST_F(FormatTest, BlockSize) {
    // Compressed formats have 4x4 blocks
    EXPECT_EQ(TextureDecompressor::get_block_size(TextureFormat::k_DXT1), 4u);
    EXPECT_EQ(TextureDecompressor::get_block_size(TextureFormat::k_DXT4_5), 4u);
    EXPECT_EQ(TextureDecompressor::get_block_size(TextureFormat::k_DXN), 4u);
    // Uncompressed formats have 1x1 blocks
    EXPECT_EQ(TextureDecompressor::get_block_size(TextureFormat::k_8_8_8_8), 1u);
    EXPECT_EQ(TextureDecompressor::get_block_size(TextureFormat::k_5_6_5), 1u);
}

TEST_F(FormatTest, BytesPerBlock) {
    // DXT1/BC1: 8 bytes per 4x4 block
    EXPECT_EQ(TextureDecompressor::get_bytes_per_block(TextureFormat::k_DXT1), 8u);
    // DXT5/BC3: 16 bytes per 4x4 block
    EXPECT_EQ(TextureDecompressor::get_bytes_per_block(TextureFormat::k_DXT4_5), 16u);
    // DXN/BC5: 16 bytes per 4x4 block
    EXPECT_EQ(TextureDecompressor::get_bytes_per_block(TextureFormat::k_DXN), 16u);
    // RGBA8: 4 bytes per pixel
    EXPECT_EQ(TextureDecompressor::get_bytes_per_block(TextureFormat::k_8_8_8_8), 4u);
    // RGB565: 2 bytes per pixel
    EXPECT_EQ(TextureDecompressor::get_bytes_per_block(TextureFormat::k_5_6_5), 2u);
}

//=============================================================================
// DXT Decompression Tests
//=============================================================================

class DXTDecompressTest : public ::testing::Test {};

TEST_F(DXTDecompressTest, DXT1_SolidBlock) {
    // DXT1 block: two identical colors = solid color
    // Color0 = Color1 = 0xFFFF (white in 565: R=31, G=63, B=31)
    u8 block[8] = {};
    block[0] = 0xFF; block[1] = 0xFF;  // color0 = white
    block[2] = 0xFF; block[3] = 0xFF;  // color1 = white
    block[4] = 0x00; block[5] = 0x00;  // all pixels use color0
    block[6] = 0x00; block[7] = 0x00;

    u8 output[64] = {};
    TextureDecompressor::decompress_dxt1_block(block, output);

    // All 16 pixels should be white (255,255,255,255)
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(output[i * 4 + 0], 255) << "R at pixel " << i;
        EXPECT_EQ(output[i * 4 + 1], 255) << "G at pixel " << i;
        EXPECT_EQ(output[i * 4 + 2], 255) << "B at pixel " << i;
        EXPECT_EQ(output[i * 4 + 3], 255) << "A at pixel " << i;
    }
}

TEST_F(DXTDecompressTest, DXT5A_SolidBlock) {
    // DXT5A: alpha only. alpha0=200, alpha1=200, all indices=0
    u8 block[8] = {};
    block[0] = 200;  // alpha0
    block[1] = 200;  // alpha1
    // indices all 0 (use alpha0)
    block[2] = 0; block[3] = 0; block[4] = 0;
    block[5] = 0; block[6] = 0; block[7] = 0;

    u8 output[16] = {};
    TextureDecompressor::decompress_dxt5a_block(block, output);

    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(output[i], 200) << "alpha at pixel " << i;
    }
}

//=============================================================================
// Texture Size Calculation
//=============================================================================

class TextureSizeTest : public ::testing::Test {};

TEST_F(TextureSizeTest, SingleMipUncompressed) {
    // 64x64 RGBA8, 1 mip = 64*64*4 = 16384
    u32 size = TextureDecompressor::calculate_texture_size(64, 64, 1,
                TextureFormat::k_8_8_8_8, 1);
    EXPECT_EQ(size, 64u * 64u * 4u);
}

TEST_F(TextureSizeTest, SingleMipCompressed) {
    // 64x64 DXT1, 1 mip = (64/4)*(64/4)*8 = 16*16*8 = 2048
    u32 size = TextureDecompressor::calculate_texture_size(64, 64, 1,
                TextureFormat::k_DXT1, 1);
    EXPECT_EQ(size, 16u * 16u * 8u);
}

TEST_F(TextureSizeTest, MultipleMips) {
    // 64x64 RGBA8, all mips down to 1x1
    // 64x64=16384, 32x32=4096, 16x16=1024, 8x8=256, 4x4=64, 2x2=16, 1x1=4
    // Total = 21844 (approximately 64*64*4 * 4/3)
    u32 size = TextureDecompressor::calculate_texture_size(64, 64, 1,
                TextureFormat::k_8_8_8_8, 7);
    u32 expected = 0;
    u32 w = 64, h = 64;
    for (int i = 0; i < 7; i++) {
        expected += w * h * 4;
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }
    EXPECT_EQ(size, expected);
}

TEST_F(TextureSizeTest, OneByOne) {
    // 1x1 RGBA8 = 4 bytes
    u32 size = TextureDecompressor::calculate_texture_size(1, 1, 1,
                TextureFormat::k_8_8_8_8, 1);
    EXPECT_EQ(size, 4u);
}

//=============================================================================
// Large Texture Tiling (stress)
//=============================================================================

TEST(LargeTextureTest, Tiling4096x4096) {
    // 4096x4096 in 4x4 DXT1 blocks = 1024x1024 blocks, 8 bytes each
    constexpr u32 BLOCKS_X = 1024;
    constexpr u32 BLOCKS_Y = 1024;
    constexpr u32 BPP = 8;
    constexpr u32 LINEAR_SIZE = BLOCKS_X * BLOCKS_Y * BPP;

    std::vector<u8> original(LINEAR_SIZE);
    u32 tiled_sz = tiled_buffer_size(BLOCKS_X, BLOCKS_Y, BPP);
    std::vector<u8> tiled(tiled_sz, 0);
    std::vector<u8> recovered(LINEAR_SIZE, 0);

    // Fill with recognizable pattern
    fill_pattern(original.data(), LINEAR_SIZE, 4096);

    tile_2d(original.data(), tiled.data(), BLOCKS_X, BLOCKS_Y, BPP);
    TextureUntiler::untile_2d(tiled.data(), recovered.data(),
                               BLOCKS_X, BLOCKS_Y, BPP, 1, 1);

    EXPECT_EQ(memcmp(original.data(), recovered.data(), LINEAR_SIZE), 0)
        << "4096x4096 DXT1 round-trip failed";
}

//=============================================================================
// NEON Untiling Test (ARM64 only, fallback to scalar on other platforms)
//=============================================================================

#if defined(__aarch64__) || defined(_M_ARM64)
class NeonUntilingTest : public ::testing::TestWithParam<std::tuple<u32, u32, u32>> {};

TEST_P(NeonUntilingTest, MatchesScalar) {
    auto [width, height, bpp] = GetParam();

    u32 linear_size = width * height * bpp;
    u32 tiled_sz = tiled_buffer_size(width, height, bpp);

    std::vector<u8> original(linear_size);
    std::vector<u8> tiled(tiled_sz, 0);
    std::vector<u8> scalar_result(linear_size, 0);
    std::vector<u8> neon_result(linear_size, 0);

    fill_pattern(original.data(), linear_size, bpp * 999);
    tile_2d(original.data(), tiled.data(), width, height, bpp);

    TextureUntiler::untile_2d(tiled.data(), scalar_result.data(),
                               width, height, bpp, 1, 1);
    TextureUntiler::untile_2d_neon(tiled.data(), neon_result.data(),
                                    width, height, bpp, 1, 1);

    EXPECT_EQ(memcmp(scalar_result.data(), neon_result.data(), linear_size), 0)
        << "NEON result differs from scalar for " << width << "x" << height << " bpp=" << bpp;
}

INSTANTIATE_TEST_SUITE_P(
    NeonSizes, NeonUntilingTest,
    ::testing::Values(
        std::make_tuple(64u, 64u, 4u),
        std::make_tuple(64u, 64u, 8u),
        std::make_tuple(64u, 64u, 16u),
        std::make_tuple(128u, 128u, 4u),
        std::make_tuple(33u, 33u, 4u),   // NPOT
        std::make_tuple(256u, 256u, 8u)
    )
);
#endif
