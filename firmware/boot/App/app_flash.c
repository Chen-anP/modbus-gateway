/*
 * app_flash.c - 新固件写进 App 区，说明见 app_flash.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "crc32.h"
#include "boot_cfg.h"
#include "flash_layout.h"
#include "app_flash.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

af_err_t app_flash_check(const af_layout_t *l, const uint8_t *img, uint32_t len, uint32_t load_addr)
{
    if (load_addr != l->app_addr)
        return AF_ERR_ADDR;
    if (len < 16u || len > l->app_max)
        return AF_ERR_SIZE;
    if (!boot_vectors_ok(rd32(&img[0]), rd32(&img[4]), l->app_addr, len, l->ram_start, l->ram_end))
        return AF_ERR_VECTORS;
    return AF_OK;
}

static int write_cfg(const flash_io_t *io, const af_layout_t *l, uint32_t len, uint32_t crc, uint32_t ver)
{
    boot_cfg_t c;

    memset(&c, 0, sizeof(c));
    c.app_len = len;
    c.app_crc = crc;
    c.app_ver = ver;
    c.stay    = 1;
    return boot_cfg_write(io, l->cfg_addr, &c);
}

af_err_t app_flash_program(const flash_io_t *io, const af_layout_t *l,
                           const uint8_t *img, uint32_t len, uint32_t crc, uint32_t ver)
{
    uint8_t blk[16];
    uint32_t off, full, sectors, i;

    if (len < 16u || len > l->app_max)
        return AF_ERR_SIZE;

    /* 1. 先让 Bootloader 在升级完成前一直留下 */
    if (write_cfg(io, l, 0, 0, 0) != 0)
        return AF_ERR_CFG;

    /* 2. 擦扇区：第一个扇区（向量表所在）最先擦 */
    sectors = (len + FL_SECTOR_SIZE - 1u) / FL_SECTOR_SIZE;
    for (i = 0; i < sectors; i++)
        if (io->erase(l->app_addr + i * FL_SECTOR_SIZE) != 0)
            return AF_ERR_ERASE;

    /* 3. 第 16 字节以后：整块的一次写完，最后不满 16 字节的补 0xFF */
    full = len & ~15u;
    if (full > 16u && io->write(l->app_addr + 16u, &img[16], full - 16u) != 0)
        return AF_ERR_WRITE;
    if (full < len) {
        memset(blk, 0xFF, sizeof(blk));
        memcpy(blk, &img[full], len - full);
        if (io->write(l->app_addr + full, blk, 16) != 0)
            return AF_ERR_WRITE;
    }

    /* 4. 核对：前 16 字节还没写，用 RAM 里的代替 */
    off = crc32_update(0, img, 16);
    if (crc32_update(off, io->ptr(l->app_addr + 16u), len - 16u) != crc)
        return AF_ERR_VERIFY;

    /* 5. 最后写向量表 */
    if (io->write(l->app_addr, img, 16) != 0)
        return AF_ERR_WRITE;

    /* 6. 整个 App 再核对一次 */
    if (crc32_update(0, io->ptr(l->app_addr), len) != crc)
        return AF_ERR_VERIFY;

    /* 7. 记下新 App 的长度和 CRC；stay 还是 1，等 0x0A00 */
    if (write_cfg(io, l, len, crc, ver) != 0)
        return AF_ERR_CFG;
    return AF_OK;
}

const char *af_err_name(af_err_t e)
{
    static const char *const k[] = { "ok", "load address", "size", "vectors", "erase", "write", "verify", "config" };
    return ((unsigned)e < sizeof(k) / sizeof(k[0])) ? k[e] : "?";
}
