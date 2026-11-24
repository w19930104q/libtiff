/*
 * Copyright (c) 2026, Libtiff contributors.
 *
 * Regression test (scenario "LT-C") for custom I/O callback short reads and
 * read errors on tiled images.
 *
 * A tiled, PackBits-compressed image with four tiles is held in a memory
 * buffer and accessed through TIFFClientOpen() with custom read/seek/size/
 * close callbacks (opened with mode "rm" so the dummy map proc is never
 * used).
 *
 * The read callback injects a fault for the third tile (index 2):
 *   - the first read covering that tile returns only half of the requested
 *     byte count (a short read);
 *   - every subsequent read at that position fails with -1 / EIO.
 *
 * Both the encoded tile decoder entry point (TIFFReadEncodedTile) and the
 * raw file entry point (TIFFReadRawTile) must converge to the same state:
 *   - a -1 return code after the short read;
 *   - the caller's pixel buffer keeps the bytes it was pre-filled with
 *     (the 0xCC pattern) instead of being zeroed or mixed with another
 *     tile's data;
 *   - re-seeking to the faulty tile fails again with the same error;
 *   - the remaining tiles stay readable;
 *   - the client close callback is invoked exactly once.
 */

#include "tiffiop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LT_TILEW 16u
#define LT_TILEH 16u
#define LT_IMGW 32u
#define LT_IMGH 32u
#define LT_NTILES 4u
#define LT_FILL 0xCCu
#define LT_FAULT_TILE 2u

typedef struct mem_file
{
    uint8_t *data;
    tmsize_t size;
    tmsize_t pos;
    /* Reads fully inside [fault_start, fault_end) are subject to the
     * injected fault. */
    uint64_t fault_start;
    uint64_t fault_end;
    int short_read_done;
    int eio_count;
    int close_count;
} mem_file;

static tmsize_t mem_read(thandle_t handle, void *buf, tmsize_t n)
{
    mem_file *mf = (mem_file *)handle;
    uint64_t pos = (uint64_t)mf->pos;

    if (pos >= mf->fault_start && pos < mf->fault_end)
    {
        if (!mf->short_read_done)
        {
            tmsize_t half = n / 2;
            if (half == 0)
                half = 1;
            if (mf->pos + half > mf->size)
                half = mf->size - mf->pos;
            memcpy(buf, mf->data + mf->pos, (size_t)half);
            mf->pos += half;
            mf->short_read_done = 1;
            return half;
        }
        mf->eio_count++;
        errno = EIO;
        return (tmsize_t)-1;
    }

    if (mf->pos >= mf->size)
        return 0;
    if (mf->pos + n > mf->size)
        n = mf->size - mf->pos;
    memcpy(buf, mf->data + mf->pos, (size_t)n);
    mf->pos += n;
    return n;
}

static tmsize_t mem_write(thandle_t handle, void *buf, tmsize_t n)
{
    mem_file *mf = (mem_file *)handle;
    if (mf->pos + n > mf->size)
    {
        tmsize_t oldsize = mf->size;
        tmsize_t newsize = mf->pos + n;
        uint8_t *newdata = (uint8_t *)realloc(mf->data, (size_t)newsize);
        if (newdata == NULL)
            return (tmsize_t)-1;
        /* Zero-fill holes created by seeks beyond the current end. */
        memset(newdata + oldsize, 0, (size_t)(newsize - oldsize));
        mf->data = newdata;
        mf->size = newsize;
    }
    memcpy(mf->data + mf->pos, buf, (size_t)n);
    mf->pos += n;
    return n;
}

static toff_t mem_seek(thandle_t handle, toff_t off, int whence)
{
    mem_file *mf = (mem_file *)handle;
    tmsize_t newpos;
    if (whence == SEEK_SET)
        newpos = (tmsize_t)off;
    else if (whence == SEEK_CUR)
        newpos = mf->pos + (tmsize_t)off;
    else
        newpos = mf->size + (tmsize_t)off;
    if (newpos < 0)
        return (toff_t)-1;
    mf->pos = newpos;
    return (toff_t)newpos;
}

static int mem_close(thandle_t handle)
{
    mem_file *mf = (mem_file *)handle;
    mf->close_count++;
    return 0;
}

static toff_t mem_size(thandle_t handle)
{
    mem_file *mf = (mem_file *)handle;
    return (toff_t)mf->size;
}

static int mem_map(thandle_t handle, void **base, toff_t *size)
{
    (void)handle;
    (void)base;
    (void)size;
    /* Refuse mapping so all I/O goes through the read/seek callbacks. */
    return -1;
}

static void mem_unmap(thandle_t handle, void *base, toff_t size)
{
    (void)handle;
    (void)base;
    (void)size;
}

static void silent_handler(const char *module, const char *fmt, va_list ap)
{
    (void)module;
    (void)fmt;
    (void)ap;
}

static int build_tiled_image(mem_file *mf)
{
    TIFF *tif = TIFFClientOpen("ltc", "w", (thandle_t)mf, mem_read, mem_write,
                               mem_seek, mem_close, mem_size, mem_map,
                               mem_unmap);
    if (tif == NULL)
        return -1;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, LT_IMGW);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, LT_IMGH);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, LT_TILEW);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, LT_TILEH);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

    uint8_t tile[LT_TILEW * LT_TILEH];
    memset(tile, LT_FILL, sizeof(tile));
    for (uint32_t t = 0; t < LT_NTILES; t++)
    {
        if (TIFFWriteEncodedTile(tif, t, tile, (tmsize_t)sizeof(tile)) ==
            (tmsize_t)-1)
        {
            TIFFClose(tif);
            return -2;
        }
    }
    TIFFClose(tif);
    mf->close_count = 0;
    return 0;
}

static int all_fill(const uint8_t *buf, tmsize_t size)
{
    for (tmsize_t i = 0; i < size; i++)
    {
        if (buf[i] != LT_FILL)
            return 0;
    }
    return 1;
}

int main(void)
{
    int failures = 0;
    mem_file mf;
    uint64_t fault_start, fault_end;
    TIFF *tif;
    tmsize_t tilesize;
    uint8_t *buf;

    memset(&mf, 0, sizeof(mf));
    TIFFSetErrorHandler(silent_handler);
    TIFFSetWarningHandler(silent_handler);

    if (build_tiled_image(&mf) != 0)
    {
        fprintf(stderr, "FAILED: cannot build in-memory tiled image\n");
        return 1;
    }

    /* Locate the faulty tile's byte range with a fault-free handle. */
    {
        mem_file probe = mf;
        probe.pos = 0;
        probe.fault_start = 0;
        probe.fault_end = 0;
        TIFF *ptif = TIFFClientOpen("ltc", "rm", (thandle_t)&probe, mem_read,
                                    mem_write, mem_seek, mem_close, mem_size,
                                    mem_map, mem_unmap);
        if (ptif == NULL)
        {
            fprintf(stderr, "FAILED: cannot open probe handle\n");
            free(mf.data);
            return 1;
        }
        if (TIFFNumberOfTiles(ptif) != LT_NTILES)
        {
            fprintf(stderr, "FAILED: expected %u tiles\n", LT_NTILES);
            failures++;
        }
        fault_start = TIFFGetStrileOffset(ptif, LT_FAULT_TILE);
        fault_end = fault_start +
                    TIFFGetStrileByteCount(ptif, LT_FAULT_TILE);
        TIFFClose(ptif);
    }

    /* Arm the LT-C fault on the third tile. */
    mf.fault_start = fault_start;
    mf.fault_end = fault_end;
    mf.pos = 0;

    tif = TIFFClientOpen("ltc", "rm", (thandle_t)&mf, mem_read, mem_write,
                         mem_seek, mem_close, mem_size, mem_map, mem_unmap);
    if (tif == NULL)
    {
        fprintf(stderr, "FAILED: cannot open faulty handle\n");
        free(mf.data);
        return 1;
    }

    tilesize = TIFFTileSize(tif);
    if (tilesize != (tmsize_t)(LT_TILEW * LT_TILEH))
    {
        fprintf(stderr, "FAILED: unexpected tile size %zd\n", tilesize);
        failures++;
    }
    buf = (uint8_t *)malloc((size_t)tilesize);
    if (buf == NULL)
    {
        TIFFClose(tif);
        free(mf.data);
        return 1;
    }

    /* Tiles 0 and 1 must decode normally. */
    for (uint32_t t = 0; t < LT_FAULT_TILE; t++)
    {
        memset(buf, LT_FILL, (size_t)tilesize);
        if (TIFFReadEncodedTile(tif, t, buf, tilesize) != tilesize ||
            !all_fill(buf, tilesize))
        {
            fprintf(stderr, "FAILED: tile %u should decode cleanly\n", t);
            failures++;
        }
    }

    /* Third tile: short read on the first callback, then EIO. The encoded
     * entry point must return -1 and preserve the 0xCC pixel pattern. */
    memset(buf, LT_FILL, (size_t)tilesize);
    if (TIFFReadEncodedTile(tif, LT_FAULT_TILE, buf, tilesize) !=
        (tmsize_t)-1)
    {
        fprintf(stderr, "FAILED: encoded tile %u must return -1\n",
                LT_FAULT_TILE);
        failures++;
    }
    if (!mf.short_read_done)
    {
        fprintf(stderr, "FAILED: expected a half-size short read\n");
        failures++;
    }
    if (!all_fill(buf, tilesize))
    {
        fprintf(stderr,
                "FAILED: pixel buffer must keep the 0xCC pattern\n");
        failures++;
    }

    /* Re-seek to the faulty tile: the next callback reports EIO and the
     * same -1 result must be produced, again without touching the buffer. */
    int eio_before = mf.eio_count;
    memset(buf, LT_FILL, (size_t)tilesize);
    if (TIFFReadEncodedTile(tif, LT_FAULT_TILE, buf, tilesize) !=
        (tmsize_t)-1)
    {
        fprintf(stderr, "FAILED: retry on tile %u must return -1\n",
                LT_FAULT_TILE);
        failures++;
    }
    if (mf.eio_count != eio_before + 1)
    {
        fprintf(stderr,
                "FAILED: retry must surface the same EIO callback error\n");
        failures++;
    }
    if (!all_fill(buf, tilesize))
    {
        fprintf(stderr, "FAILED: retry must preserve the 0xCC pattern\n");
        failures++;
    }

    /* The fourth tile must remain readable. */
    memset(buf, LT_FILL, (size_t)tilesize);
    if (TIFFReadEncodedTile(tif, LT_NTILES - 1, buf, tilesize) != tilesize ||
        !all_fill(buf, tilesize))
    {
        fprintf(stderr, "FAILED: tile %u should still decode\n",
                LT_NTILES - 1);
        failures++;
    }

    /* The raw file entry point must converge to the same state: -1 after
     * the short read / EIO, without overwriting bytes the caller pre-filled
     * beyond what the callback itself delivered. */
    memset(buf, LT_FILL, (size_t)tilesize);
    if (TIFFReadRawTile(tif, LT_FAULT_TILE, buf, (tmsize_t)-1) !=
        (tmsize_t)-1)
    {
        fprintf(stderr, "FAILED: raw tile %u must return -1\n",
                LT_FAULT_TILE);
        failures++;
    }
    {
        /* The callback writes at most half of the (compressed) tile; the
         * tail of this 256-byte buffer must still be the 0xCC pattern, i.e.
         * no data from another tile may leak in. */
        int tail_ok = 1;
        for (tmsize_t i = tilesize / 2; i < tilesize; i++)
        {
            if (buf[i] != LT_FILL)
            {
                tail_ok = 0;
                break;
            }
        }
        if (!tail_ok)
        {
            fprintf(stderr,
                    "FAILED: raw tile failure must not mix in other data\n");
            failures++;
        }
    }

    TIFFClose(tif);
    if (mf.close_count != 1)
    {
        fprintf(stderr,
                "FAILED: close callback must run once, got %d\n",
                mf.close_count);
        failures++;
    }

    free(buf);
    free(mf.data);

    if (failures)
    {
        fprintf(stderr, "%d LT-C check(s) failed\n", failures);
        return 1;
    }
    printf("LT-C custom callback short-read/EIO tile test passed\n");
    return 0;
}
