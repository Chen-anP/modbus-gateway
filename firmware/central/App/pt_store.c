/*
 * pt_store.c - 点表存储，说明见 pt_store.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "crc32.h"
#include "pt_store.h"

/* 头：8 个 32 位字，小端（和 CPU 一样，直接按 uint32_t 读写） */
enum { H_MAGIC, H_FMT, H_LEN, H_DCRC, H_SEQ, H_RSV1, H_RSV2, H_HCRC };

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

pts_result_t pt_store_load(const flash_io_t *io, uint32_t addr, const uint8_t **data, uint32_t *len, uint32_t *seq)
{
    const uint8_t *h = io->ptr(addr);
    uint32_t n;

    if (rd32(&h[H_MAGIC * 4]) != PT_STORE_MAGIC || rd32(&h[H_FMT * 4]) != 1u)
        return PTS_EMPTY;
    if (crc32_update(0, h, PT_STORE_HDR_SIZE - 4) != rd32(&h[H_HCRC * 4]))
        return PTS_EMPTY;                       /* 写头时断电 */
    n = rd32(&h[H_LEN * 4]);
    if (n == 0 || n > PT_STORE_MAX_DATA)
        return PTS_BAD_DATA;
    if (crc32_update(0, io->ptr(addr + PT_STORE_HDR_SIZE), n) != rd32(&h[H_DCRC * 4]))
        return PTS_BAD_DATA;
    *data = io->ptr(addr + PT_STORE_HDR_SIZE);
    *len = n;
    if (seq)
        *seq = rd32(&h[H_SEQ * 4]);
    return PTS_OK;
}

pts_result_t pt_store_save(const flash_io_t *io, uint32_t addr, const uint8_t *data, uint32_t len, uint32_t seq)
{
    uint8_t blk[16], hdr[PT_STORE_HDR_SIZE];
    uint32_t off;

    if (len == 0 || len > PT_STORE_MAX_DATA)
        return PTS_TOO_BIG;
    if (io->erase(addr) != 0)
        return PTS_FLASH_ERR;

    /* 1. 数据：每次 16 字节，最后一块不满的部分补 0xFF（和擦除后的状态一样） */
    for (off = 0; off < len; off += 16) {
        uint32_t n = (len - off) < 16 ? (len - off) : 16;
        memset(blk, 0xFF, sizeof(blk));
        memcpy(blk, &data[off], n);
        if (io->write(addr + PT_STORE_HDR_SIZE + off, blk, 16) != 0)
            return PTS_FLASH_ERR;
    }

    /* 2. 最后写头：有了头，才说明数据是完整的 */
    memset(hdr, 0, sizeof(hdr));
    wr32(&hdr[H_MAGIC * 4], PT_STORE_MAGIC);
    wr32(&hdr[H_FMT * 4], 1u);
    wr32(&hdr[H_LEN * 4], len);
    wr32(&hdr[H_DCRC * 4], crc32_update(0, data, len));
    wr32(&hdr[H_SEQ * 4], seq);
    wr32(&hdr[H_HCRC * 4], crc32_update(0, hdr, PT_STORE_HDR_SIZE - 4));
    if (io->write(addr, hdr, PT_STORE_HDR_SIZE) != 0)
        return PTS_FLASH_ERR;
    return PTS_OK;
}

pts_result_t pt_store_clear(const flash_io_t *io, uint32_t addr)
{
    return io->erase(addr) == 0 ? PTS_OK : PTS_FLASH_ERR;
}

const char *pts_name(pts_result_t r)
{
    static const char *const k[] = { "ok", "empty", "bad data", "too big", "flash error" };
    return ((unsigned)r < sizeof(k) / sizeof(k[0])) ? k[r] : "?";
}
