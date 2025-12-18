/* libmspack -- a library for working with Microsoft compression formats
 * (C) 2003-2019 Stuart Caie <kyzer@cabextract.org.uk>
 *
 * libmspack is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License (LGPL) version 2.1
 *
 * LZX Decompression implementation
 * Modified for 360μ emulator integration.
 */

#include "mspack.h"
#include <string.h>
#include <stdlib.h>

/* LZX constants */
#define LZX_MIN_MATCH              (2)
#define LZX_MAX_MATCH              (257)
#define LZX_NUM_CHARS              (256)
#define LZX_BLOCKTYPE_INVALID      (0)
#define LZX_BLOCKTYPE_VERBATIM     (1)
#define LZX_BLOCKTYPE_ALIGNED      (2)
#define LZX_BLOCKTYPE_UNCOMPRESSED (3)
#define LZX_PRETREE_NUM_ELEMENTS   (20)
#define LZX_ALIGNED_NUM_ELEMENTS   (8)
#define LZX_NUM_PRIMARY_LENGTHS    (7)
#define LZX_NUM_SECONDARY_LENGTHS  (249)

/* LZX Huffman constants */
#define LZX_PRETREE_MAXSYMBOLS     (LZX_PRETREE_NUM_ELEMENTS)
#define LZX_PRETREE_TABLEBITS      (6)
#define LZX_MAINTREE_MAXSYMBOLS    (LZX_NUM_CHARS + 290)
#define LZX_MAINTREE_TABLEBITS     (12)
#define LZX_LENGTH_MAXSYMBOLS      (LZX_NUM_SECONDARY_LENGTHS + 1)
#define LZX_LENGTH_TABLEBITS       (12)
#define LZX_ALIGNED_MAXSYMBOLS     (LZX_ALIGNED_NUM_ELEMENTS)
#define LZX_ALIGNED_TABLEBITS      (7)
#define LZX_LENTABLE_SAFETY        (64)

#define HUFF_MAXBITS 16

/* Position slot base and extra bits tables */
static const unsigned int position_base[51] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288,
    16384, 24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144, 393216,
    524288, 655360, 786432, 917504, 1048576, 1179648, 1310720, 1441792, 1572864,
    1703936, 1835008, 1966080, 2097152
};

static const unsigned char extra_bits[51] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
    15, 15, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17
};

/* LZX decompressor state */
struct lzxd_stream {
    struct mspack_system *sys;
    struct mspack_file *input;
    struct mspack_file *output;

    off_t offset;              /* offset within output */
    off_t length;              /* total expected output length */

    unsigned char *window;     /* sliding window buffer */
    unsigned int window_size;  /* window size (32KB-2MB) */
    unsigned int window_posn;  /* current position in window */
    unsigned int frame_posn;   /* current position in frame */
    unsigned int frame;        /* current frame number */
    unsigned int reset_interval; /* frames between state resets (0=never) */

    int is_delta;              /* delta LZX stream? */
    int intel_filesize;        /* uncompressed file size for E8 fixup */
    int intel_curpos;          /* current output position for E8 fixup */
    int intel_started;         /* E8 processing started? */

    /* I/O buffering */
    unsigned char *inbuf;
    unsigned int inbuf_size;
    unsigned char *i_ptr;
    unsigned char *i_end;
    
    /* Bitstream state */
    unsigned int bit_buffer;
    unsigned int bits_left;
    int input_end;

    /* LZX decode state */
    unsigned int R0, R1, R2;   /* repeated offset history */
    unsigned int block_length; /* uncompressed bytes remaining in block */
    unsigned int block_remaining; /* remaining blocks in frame */
    unsigned char block_type;  /* type of current block */
    unsigned char header_read; /* has the header been read? */
    unsigned int num_offsets;  /* number of match offset position slots */

    /* Huffman tables */
    unsigned short PRETREE_table[(1 << LZX_PRETREE_TABLEBITS) + (LZX_PRETREE_MAXSYMBOLS * 2)];
    unsigned char PRETREE_len[LZX_PRETREE_MAXSYMBOLS + LZX_LENTABLE_SAFETY];
    
    unsigned short MAINTREE_table[(1 << LZX_MAINTREE_TABLEBITS) + (LZX_MAINTREE_MAXSYMBOLS * 2)];
    unsigned char MAINTREE_len[LZX_MAINTREE_MAXSYMBOLS + LZX_LENTABLE_SAFETY];
    
    unsigned short LENGTH_table[(1 << LZX_LENGTH_TABLEBITS) + (LZX_LENGTH_MAXSYMBOLS * 2)];
    unsigned char LENGTH_len[LZX_LENGTH_MAXSYMBOLS + LZX_LENTABLE_SAFETY];
    
    unsigned short ALIGNED_table[(1 << LZX_ALIGNED_TABLEBITS) + (LZX_ALIGNED_MAXSYMBOLS * 2)];
    unsigned char ALIGNED_len[LZX_ALIGNED_MAXSYMBOLS + LZX_LENTABLE_SAFETY];

    int error;
};

/* Read more input */
static int lzxd_read_input(struct lzxd_stream *lzx) {
    int read = lzx->sys->read(lzx->input, lzx->inbuf, (int)lzx->inbuf_size);
    if (read < 0) return lzx->error = MSPACK_ERR_READ;
    /* Always read in pairs */
    if (read & 1) {
        if (lzx->sys->read(lzx->input, &lzx->inbuf[read], 1) != 1) {
            return lzx->error = MSPACK_ERR_READ;
        }
        read++;
    }
    lzx->i_ptr = &lzx->inbuf[0];
    lzx->i_end = &lzx->inbuf[read];
    return MSPACK_ERR_OK;
}

/* Ensure we have enough bits in the buffer */
static int lzxd_ensure_bits(struct lzxd_stream *lzx, unsigned int nbits) {
    while (lzx->bits_left < nbits) {
        if (lzx->i_ptr >= lzx->i_end) {
            if (lzxd_read_input(lzx) != MSPACK_ERR_OK) return lzx->error;
            if (lzx->i_ptr >= lzx->i_end) {
                lzx->input_end = 1;
                return MSPACK_ERR_OK;
            }
        }
        lzx->bit_buffer |= ((unsigned int)lzx->i_ptr[1] << 8 | lzx->i_ptr[0])
                           << (sizeof(lzx->bit_buffer) * 8 - 16 - lzx->bits_left);
        lzx->i_ptr += 2;
        lzx->bits_left += 16;
    }
    return MSPACK_ERR_OK;
}

/* Peek bits without removing them */
static unsigned int lzxd_peek_bits(struct lzxd_stream *lzx, unsigned int nbits) {
    return lzx->bit_buffer >> (sizeof(lzx->bit_buffer) * 8 - nbits);
}

/* Remove bits from buffer */
static void lzxd_remove_bits(struct lzxd_stream *lzx, unsigned int nbits) {
    lzx->bit_buffer <<= nbits;
    lzx->bits_left -= nbits;
}

/* Read bits from buffer */
static unsigned int lzxd_read_bits(struct lzxd_stream *lzx, unsigned int nbits) {
    lzxd_ensure_bits(lzx, nbits);
    unsigned int val = lzxd_peek_bits(lzx, nbits);
    lzxd_remove_bits(lzx, nbits);
    return val;
}

/* Build a Huffman decode table */
static int make_decode_table(unsigned int nsyms, unsigned int nbits,
                              unsigned char *length, unsigned short *table)
{
    unsigned short sym;
    unsigned int leaf, fill;
    unsigned int bit_num, table_mask, pos;

    /* Calculate table size */
    table_mask = 1u << nbits;

    /* Initialize with direct entries */
    pos = 0;
    for (bit_num = 1; bit_num <= nbits; bit_num++) {
        for (sym = 0; sym < nsyms; sym++) {
            if (length[sym] == bit_num) {
                /* fill table with direct entries */
                leaf = pos;
                fill = table_mask >> bit_num;
                if ((pos += fill) > table_mask) return 1;
                while (fill-- > 0) table[leaf++] = sym;
            }
        }
    }

    /* If table not full, mark remaining entries and build tree for longer codes */
    if (pos != table_mask) {
        unsigned int next_symbol = table_mask;
        
        for (sym = (unsigned short)pos; sym < table_mask; sym++) {
            table[sym] = 0xFFFF;
        }
        
        for (bit_num = nbits + 1; bit_num <= HUFF_MAXBITS; bit_num++) {
            for (sym = 0; sym < nsyms; sym++) {
                if (length[sym] == bit_num) {
                    unsigned int code_pos = pos >> (bit_num - nbits);
                    
                    /* Build tree nodes as needed */
                    for (unsigned int k = nbits; k < bit_num; k++) {
                        if (table[code_pos] == 0xFFFF) {
                            table[next_symbol] = 0xFFFF;
                            table[next_symbol + 1] = 0xFFFF;
                            table[code_pos] = (unsigned short)next_symbol;
                            next_symbol += 2;
                        }
                        code_pos = table[code_pos] + ((pos >> (bit_num - k - 1)) & 1);
                    }
                    table[code_pos] = sym;
                    pos += (1u << (HUFF_MAXBITS - bit_num));
                }
            }
        }
    }
    return (pos != (1u << HUFF_MAXBITS) && pos != 0) ? 1 : 0;
}

/* Decode a Huffman symbol */
static int lzxd_decode_huffman(struct lzxd_stream *lzx, 
                                unsigned short *table, unsigned char *lengths,
                                unsigned int tablebits, unsigned int maxsyms,
                                unsigned int *result) {
    unsigned int sym;
    unsigned int j;
    
    lzxd_ensure_bits(lzx, HUFF_MAXBITS);
    sym = table[lzxd_peek_bits(lzx, tablebits)];
    
    if (sym >= maxsyms) {
        j = 1u << (sizeof(lzx->bit_buffer) * 8 - tablebits);
        do {
            j >>= 1;
            sym = table[(sym << 1) | ((lzx->bit_buffer & j) ? 1 : 0)];
        } while (sym >= maxsyms);
    }
    
    *result = sym;
    lzxd_remove_bits(lzx, lengths[sym]);
    return MSPACK_ERR_OK;
}

/* Read a Huffman-encoded code length list */
static int lzxd_read_lens(struct lzxd_stream *lzx, unsigned char *lens,
                           unsigned int first, unsigned int last)
{
    unsigned int x, y, z;

    /* Read pre-tree lengths */
    for (x = 0; x < LZX_PRETREE_NUM_ELEMENTS; x++) {
        y = lzxd_read_bits(lzx, 4);
        lzx->PRETREE_len[x] = (unsigned char)y;
    }
    if (make_decode_table(LZX_PRETREE_MAXSYMBOLS, LZX_PRETREE_TABLEBITS, 
                          lzx->PRETREE_len, lzx->PRETREE_table)) {
        return lzx->error = MSPACK_ERR_DATAFORMAT;
    }

    /* Read lengths */
    for (x = first; x < last; ) {
        lzxd_decode_huffman(lzx, lzx->PRETREE_table, lzx->PRETREE_len,
                           LZX_PRETREE_TABLEBITS, LZX_PRETREE_MAXSYMBOLS, &z);
        
        if (z == 17) {
            /* zeros */
            y = lzxd_read_bits(lzx, 4) + 4;
            while (y-- && x < last) lens[x++] = 0;
        }
        else if (z == 18) {
            /* many zeros */
            y = lzxd_read_bits(lzx, 5) + 20;
            while (y-- && x < last) lens[x++] = 0;
        }
        else if (z == 19) {
            /* same as last */
            y = lzxd_read_bits(lzx, 1) + 4;
            lzxd_decode_huffman(lzx, lzx->PRETREE_table, lzx->PRETREE_len,
                               LZX_PRETREE_TABLEBITS, LZX_PRETREE_MAXSYMBOLS, &z);
            z = (lens[x] + 17 - z) % 17;
            while (y-- && x < last) lens[x++] = (unsigned char)z;
        }
        else {
            z = (lens[x] + 17 - z) % 17;
            lens[x++] = (unsigned char)z;
        }
    }
    return MSPACK_ERR_OK;
}

/* Initialize LZX decompressor */
struct lzxd_stream *lzxd_init(struct mspack_system *sys,
                               struct mspack_file *input,
                               struct mspack_file *output,
                               int window_bits,
                               int reset_interval,
                               int input_buffer_size,
                               off_t output_length,
                               int is_delta)
{
    struct lzxd_stream *lzx;
    unsigned int window_size;

    if (!sys || !input || !output) return NULL;
    if (window_bits < 15 || window_bits > 21) return NULL;

    /* Calculate window size */
    window_size = 1u << window_bits;

    /* Calculate number of position slots */
    unsigned int num_offsets;
    if (window_bits < 20) {
        num_offsets = (unsigned int)window_bits * 2;
    }
    else if (window_bits == 20) {
        num_offsets = 42;
    }
    else {
        num_offsets = 50;
    }

    /* Allocate structure */
    lzx = (struct lzxd_stream *)sys->alloc(sys, sizeof(struct lzxd_stream));
    if (!lzx) return NULL;
    
    memset(lzx, 0, sizeof(struct lzxd_stream));

    /* Allocate buffers */
    lzx->window = (unsigned char *)sys->alloc(sys, window_size);
    lzx->inbuf = (unsigned char *)sys->alloc(sys, (size_t)input_buffer_size + 2);
    if (!lzx->window || !lzx->inbuf) {
        if (lzx->window) sys->free(lzx->window);
        if (lzx->inbuf) sys->free(lzx->inbuf);
        sys->free(lzx);
        return NULL;
    }

    /* Initialize state */
    lzx->sys = sys;
    lzx->input = input;
    lzx->output = output;
    lzx->offset = 0;
    lzx->length = output_length;

    lzx->window_size = window_size;
    lzx->window_posn = 0;
    lzx->frame_posn = 0;
    lzx->frame = 0;
    lzx->reset_interval = (unsigned int)reset_interval;

    lzx->is_delta = is_delta;
    lzx->intel_filesize = 0;
    lzx->intel_curpos = 0;
    lzx->intel_started = 0;

    lzx->inbuf_size = (unsigned int)input_buffer_size;
    lzx->num_offsets = num_offsets;
    lzx->error = MSPACK_ERR_OK;

    /* Initialize bitstream */
    lzx->i_ptr = lzx->i_end = &lzx->inbuf[0];
    lzx->bit_buffer = 0;
    lzx->bits_left = 0;
    lzx->input_end = 0;

    /* Initialize match history */
    lzx->R0 = 1;
    lzx->R1 = 1;
    lzx->R2 = 1;

    lzx->block_remaining = 0;
    lzx->block_type = LZX_BLOCKTYPE_INVALID;
    lzx->header_read = 0;

    /* Clear window */
    memset(lzx->window, 0, window_size);

    return lzx;
}

/* Set reference data for delta decompression */
int lzxd_set_reference_data(struct lzxd_stream *lzx,
                             struct mspack_system *sys,
                             struct mspack_file *ref,
                             unsigned int ref_len)
{
    if (!lzx) return MSPACK_ERR_ARGS;
    if (lzx->is_delta) {
        /* Read reference data into window */
        if (ref_len > lzx->window_size) {
            ref_len = lzx->window_size;
        }
        lzx->window_posn = ref_len;
        if (sys->read(ref, lzx->window, (int)ref_len) != (int)ref_len) {
            return MSPACK_ERR_READ;
        }
    }
    return MSPACK_ERR_OK;
}

/* Decompress data */
int lzxd_decompress(struct lzxd_stream *lzx, off_t out_bytes) {
    unsigned int R0, R1, R2;
    unsigned int match_offset, match_length;
    unsigned int window_posn, this_run;
    unsigned char *window, *rundest;
    unsigned int sym, j;
    int i;

    if (!lzx) return MSPACK_ERR_ARGS;
    if (out_bytes < 0) return MSPACK_ERR_ARGS;

    /* Restore match offsets and window position */
    R0 = lzx->R0;
    R1 = lzx->R1;
    R2 = lzx->R2;
    window = lzx->window;
    window_posn = lzx->window_posn;

    /* Main decompression loop */
    while (out_bytes > 0) {
        /* Read block header if needed */
        if (lzx->block_remaining == 0) {
            /* Ensure we have enough bits for block header */
            if (lzx->input_end) break;
            
            /* Read block type (3 bits) */
            lzx->block_type = (unsigned char)lzxd_read_bits(lzx, 3);
            
            /* Read block length (24 bits) */
            lzx->block_length = lzxd_read_bits(lzx, 24);
            lzx->block_remaining = lzx->block_length;

            switch (lzx->block_type) {
            case LZX_BLOCKTYPE_ALIGNED:
                /* Read aligned offset tree */
                for (i = 0; i < LZX_ALIGNED_NUM_ELEMENTS; i++) {
                    j = lzxd_read_bits(lzx, 3);
                    lzx->ALIGNED_len[i] = (unsigned char)j;
                }
                if (make_decode_table(LZX_ALIGNED_MAXSYMBOLS, LZX_ALIGNED_TABLEBITS,
                                      lzx->ALIGNED_len, lzx->ALIGNED_table)) {
                    return lzx->error = MSPACK_ERR_DATAFORMAT;
                }
                /* Fall through to read main and length trees */
                /* FALLTHROUGH */

            case LZX_BLOCKTYPE_VERBATIM:
                /* Read main tree first 256 elements */
                if (lzxd_read_lens(lzx, lzx->MAINTREE_len, 0, LZX_NUM_CHARS)) {
                    return lzx->error;
                }
                /* Read main tree remaining elements */
                if (lzxd_read_lens(lzx, lzx->MAINTREE_len, LZX_NUM_CHARS,
                                    LZX_NUM_CHARS + lzx->num_offsets * 8)) {
                    return lzx->error;
                }
                if (make_decode_table(LZX_MAINTREE_MAXSYMBOLS, LZX_MAINTREE_TABLEBITS,
                                      lzx->MAINTREE_len, lzx->MAINTREE_table)) {
                    return lzx->error = MSPACK_ERR_DATAFORMAT;
                }
                
                /* Read length tree */
                if (lzxd_read_lens(lzx, lzx->LENGTH_len, 0, LZX_NUM_SECONDARY_LENGTHS)) {
                    return lzx->error;
                }
                if (make_decode_table(LZX_LENGTH_MAXSYMBOLS, LZX_LENGTH_TABLEBITS,
                                      lzx->LENGTH_len, lzx->LENGTH_table)) {
                    return lzx->error = MSPACK_ERR_DATAFORMAT;
                }
                break;

            case LZX_BLOCKTYPE_UNCOMPRESSED:
                /* Realign bitstream */
                if (lzx->bits_left == 0) {
                    lzxd_ensure_bits(lzx, 16);
                }
                if (lzx->bits_left >= 16) {
                    lzx->i_ptr -= 2;
                }
                lzx->bits_left = 0;
                lzx->bit_buffer = 0;
                
                /* Read match offsets from stream */
                if (lzx->i_ptr + 12 > lzx->i_end) {
                    if (lzxd_read_input(lzx) != MSPACK_ERR_OK) return lzx->error;
                    if (lzx->i_ptr + 12 > lzx->i_end) {
                        return lzx->error = MSPACK_ERR_READ;
                    }
                }
                R0 = lzx->i_ptr[0] | ((unsigned int)lzx->i_ptr[1] << 8) |
                     ((unsigned int)lzx->i_ptr[2] << 16) | ((unsigned int)lzx->i_ptr[3] << 24);
                R1 = lzx->i_ptr[4] | ((unsigned int)lzx->i_ptr[5] << 8) |
                     ((unsigned int)lzx->i_ptr[6] << 16) | ((unsigned int)lzx->i_ptr[7] << 24);
                R2 = lzx->i_ptr[8] | ((unsigned int)lzx->i_ptr[9] << 8) |
                     ((unsigned int)lzx->i_ptr[10] << 16) | ((unsigned int)lzx->i_ptr[11] << 24);
                lzx->i_ptr += 12;
                break;

            default:
                return lzx->error = MSPACK_ERR_DATAFORMAT;
            }
        }

        /* Calculate how much to decompress this iteration */
        this_run = lzx->block_remaining;
        if ((off_t)this_run > out_bytes) this_run = (unsigned int)out_bytes;
        lzx->block_remaining -= this_run;
        out_bytes -= (off_t)this_run;

        window_posn &= lzx->window_size - 1;
        if ((window_posn + this_run) > lzx->window_size) {
            return lzx->error = MSPACK_ERR_DECRUNCH;
        }

        switch (lzx->block_type) {
        case LZX_BLOCKTYPE_VERBATIM:
            while (this_run > 0) {
                lzxd_decode_huffman(lzx, lzx->MAINTREE_table, lzx->MAINTREE_len,
                                   LZX_MAINTREE_TABLEBITS, LZX_MAINTREE_MAXSYMBOLS, &sym);
                
                if (sym < LZX_NUM_CHARS) {
                    /* Literal byte */
                    window[window_posn++] = (unsigned char)sym;
                    this_run--;
                }
                else {
                    /* Match: decode length and offset */
                    sym -= LZX_NUM_CHARS;
                    match_length = sym & 7;
                    if (match_length == 7) {
                        lzxd_decode_huffman(lzx, lzx->LENGTH_table, lzx->LENGTH_len,
                                           LZX_LENGTH_TABLEBITS, LZX_LENGTH_MAXSYMBOLS, &j);
                        match_length += j;
                    }
                    match_length += LZX_MIN_MATCH;

                    /* Decode match offset */
                    match_offset = sym >> 3;
                    if (match_offset > 2) {
                        /* Not a repeated offset */
                        if (match_offset < lzx->num_offsets) {
                            unsigned int extra = extra_bits[match_offset];
                            match_offset = position_base[match_offset] - 2;
                            if (extra > 0) {
                                j = lzxd_read_bits(lzx, extra);
                                match_offset += j;
                            }
                        }
                        else {
                            match_offset = position_base[match_offset] - 2;
                        }
                        /* Update R0/R1/R2 */
                        R2 = R1; R1 = R0; R0 = match_offset;
                    }
                    else if (match_offset == 0) {
                        match_offset = R0;
                    }
                    else if (match_offset == 1) {
                        match_offset = R1;
                        R1 = R0; R0 = match_offset;
                    }
                    else {
                        match_offset = R2;
                        R2 = R0; R0 = match_offset;
                    }

                    /* Copy match */
                    rundest = &window[window_posn];
                    this_run -= match_length;
                    
                    unsigned int runsrc = (window_posn - match_offset) & (lzx->window_size - 1);
                    unsigned int copy_length = match_length;
                    window_posn += match_length;
                    
                    while (copy_length > 0) {
                        *rundest++ = window[runsrc];
                        runsrc = (runsrc + 1) & (lzx->window_size - 1);
                        copy_length--;
                    }
                }
            }
            break;

        case LZX_BLOCKTYPE_ALIGNED:
            while (this_run > 0) {
                lzxd_decode_huffman(lzx, lzx->MAINTREE_table, lzx->MAINTREE_len,
                                   LZX_MAINTREE_TABLEBITS, LZX_MAINTREE_MAXSYMBOLS, &sym);
                
                if (sym < LZX_NUM_CHARS) {
                    window[window_posn++] = (unsigned char)sym;
                    this_run--;
                }
                else {
                    sym -= LZX_NUM_CHARS;
                    match_length = sym & 7;
                    if (match_length == 7) {
                        lzxd_decode_huffman(lzx, lzx->LENGTH_table, lzx->LENGTH_len,
                                           LZX_LENGTH_TABLEBITS, LZX_LENGTH_MAXSYMBOLS, &j);
                        match_length += j;
                    }
                    match_length += LZX_MIN_MATCH;

                    match_offset = sym >> 3;
                    if (match_offset > 2) {
                        unsigned int extra = extra_bits[match_offset];
                        match_offset = position_base[match_offset] - 2;
                        if (extra > 3) {
                            j = lzxd_read_bits(lzx, extra - 3);
                            match_offset += j << 3;
                            lzxd_decode_huffman(lzx, lzx->ALIGNED_table, lzx->ALIGNED_len,
                                               LZX_ALIGNED_TABLEBITS, LZX_ALIGNED_MAXSYMBOLS, &j);
                            match_offset += j;
                        }
                        else if (extra == 3) {
                            lzxd_decode_huffman(lzx, lzx->ALIGNED_table, lzx->ALIGNED_len,
                                               LZX_ALIGNED_TABLEBITS, LZX_ALIGNED_MAXSYMBOLS, &j);
                            match_offset += j;
                        }
                        else if (extra > 0) {
                            j = lzxd_read_bits(lzx, extra);
                            match_offset += j;
                        }
                        R2 = R1; R1 = R0; R0 = match_offset;
                    }
                    else if (match_offset == 0) {
                        match_offset = R0;
                    }
                    else if (match_offset == 1) {
                        match_offset = R1;
                        R1 = R0; R0 = match_offset;
                    }
                    else {
                        match_offset = R2;
                        R2 = R0; R0 = match_offset;
                    }

                    rundest = &window[window_posn];
                    this_run -= match_length;
                    
                    unsigned int runsrc = (window_posn - match_offset) & (lzx->window_size - 1);
                    unsigned int copy_length = match_length;
                    window_posn += match_length;
                    
                    while (copy_length > 0) {
                        *rundest++ = window[runsrc];
                        runsrc = (runsrc + 1) & (lzx->window_size - 1);
                        copy_length--;
                    }
                }
            }
            break;

        case LZX_BLOCKTYPE_UNCOMPRESSED:
            /* Read uncompressed data directly */
            while (this_run > 0) {
                if (lzx->i_ptr >= lzx->i_end) {
                    if (lzxd_read_input(lzx) != MSPACK_ERR_OK) return lzx->error;
                    if (lzx->i_ptr >= lzx->i_end) {
                        return lzx->error = MSPACK_ERR_READ;
                    }
                }
                window[window_posn++] = *lzx->i_ptr++;
                this_run--;
            }
            /* Realign to 16-bit boundary if needed */
            if ((lzx->block_remaining == 0) && (lzx->block_length & 1)) {
                if (lzx->i_ptr < lzx->i_end) lzx->i_ptr++;
            }
            break;
        }
    }

    /* Store state */
    lzx->R0 = R0;
    lzx->R1 = R1;
    lzx->R2 = R2;
    lzx->window_posn = window_posn;

    /* Write output */
    if (lzx->sys->write(lzx->output, lzx->window, (int)window_posn) != (int)window_posn) {
        return lzx->error = MSPACK_ERR_WRITE;
    }

    return lzx->error;
}

/* Free LZX decompressor */
void lzxd_free(struct lzxd_stream *lzx) {
    if (lzx) {
        struct mspack_system *sys = lzx->sys;
        if (lzx->window) sys->free(lzx->window);
        if (lzx->inbuf) sys->free(lzx->inbuf);
        sys->free(lzx);
    }
}
