/* libmspack -- a library for working with Microsoft compression formats
 * (C) 2003-2019 Stuart Caie <kyzer@cabextract.org.uk>
 *
 * libmspack is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License (LGPL) version 2.1
 *
 * This file is part of libmspack.
 * Modified for 360μ emulator integration.
 */

#ifndef MSPACK_H
#define MSPACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

/* Error codes */
#define MSPACK_ERR_OK          (0)
#define MSPACK_ERR_ARGS        (1)
#define MSPACK_ERR_OPEN        (2)
#define MSPACK_ERR_READ        (3)
#define MSPACK_ERR_WRITE       (4)
#define MSPACK_ERR_SEEK        (5)
#define MSPACK_ERR_NOMEMORY    (6)
#define MSPACK_ERR_SIGNATURE   (7)
#define MSPACK_ERR_DATAFORMAT  (8)
#define MSPACK_ERR_CHECKSUM    (9)
#define MSPACK_ERR_CRUNCH      (10)
#define MSPACK_ERR_DECRUNCH    (11)

/* Seek modes for mspack_system::seek() */
#define MSPACK_SYS_SEEK_START  (0)
#define MSPACK_SYS_SEEK_CUR    (1)
#define MSPACK_SYS_SEEK_END    (2)

/* Abstract file type */
struct mspack_file;

/**
 * A structure which abstracts file I/O and memory management.
 *
 * All members are mandatory.
 */
struct mspack_system {
    /**
     * Opens a file for reading, writing, appending or updating.
     */
    struct mspack_file * (*open)(struct mspack_system *self,
                                  const char *filename,
                                  int mode);

    /**
     * Closes a previously opened file. If any memory was allocated for this
     * particular file handle, it should be freed at this time.
     */
    void (*close)(struct mspack_file *file);

    /**
     * Reads a given number of bytes from an open file.
     */
    int (*read)(struct mspack_file *file,
                void *buffer,
                int bytes);

    /**
     * Writes a given number of bytes to an open file.
     */
    int (*write)(struct mspack_file *file,
                 void *buffer,
                 int bytes);

    /**
     * Seeks to a specific file offset within an open file.
     */
    int (*seek)(struct mspack_file *file,
                off_t offset,
                int mode);

    /**
     * Returns the current file position (in bytes) of the given file.
     */
    off_t (*tell)(struct mspack_file *file);

    /**
     * Used to send messages from the library to the user.
     */
    void (*message)(struct mspack_file *file,
                    const char *format,
                    ...);

    /**
     * Allocates memory.
     */
    void * (*alloc)(struct mspack_system *self,
                    size_t bytes);

    /**
     * Frees memory.
     */
    void (*free)(void *ptr);

    /**
     * Copies from one region of memory to another.
     */
    void (*copy)(void *src,
                 void *dest,
                 size_t bytes);

    /**
     * A null pointer to mark the end of mspack_system.
     */
    void *null;
};

/* ========================================================================
 * LZX decompressor structures and functions
 * ======================================================================== */

struct lzxd_stream;

/**
 * Creates a new LZX decompressor for the given parameters.
 *
 * @param sys          mspack_system structure for I/O and memory operations
 * @param input        input stream
 * @param output       output stream
 * @param window_bits  size of history window (15-21)
 * @param reset_interval how often to reset the compressor state (0=never)
 * @param input_buffer_size size of input buffer
 * @param output_length expected output length
 * @param is_delta     is this a delta LZX stream?
 * @return pointer to LZX decompressor, or NULL on failure
 */
struct lzxd_stream *lzxd_init(struct mspack_system *sys,
                               struct mspack_file *input,
                               struct mspack_file *output,
                               int window_bits,
                               int reset_interval,
                               int input_buffer_size,
                               off_t output_length,
                               int is_delta);

/**
 * Decompresses LZX data.
 *
 * @param lzx     LZX decompressor
 * @param out_bytes number of bytes to decompress
 * @return error code (MSPACK_ERR_OK on success)
 */
int lzxd_decompress(struct lzxd_stream *lzx, off_t out_bytes);

/**
 * Frees an LZX decompressor.
 *
 * @param lzx LZX decompressor to free
 */
void lzxd_free(struct lzxd_stream *lzx);

/**
 * Sets the reference data for delta decompression.
 *
 * @param lzx      LZX decompressor
 * @param sys      mspack_system structure
 * @param ref      reference data file
 * @param ref_len  length of reference data
 * @return error code
 */
int lzxd_set_reference_data(struct lzxd_stream *lzx,
                             struct mspack_system *sys,
                             struct mspack_file *ref,
                             unsigned int ref_len);

#ifdef __cplusplus
}
#endif

#endif /* MSPACK_H */
