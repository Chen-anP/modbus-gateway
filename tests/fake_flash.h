/*
 * fake_flash.h - PC 测试用的假 Flash：一个数组 + flash_io_t 的三个函数，还能模拟断电
 *
 * 和真 Flash 一样的规则：
 *   擦除按 8 KB 扇区，擦完是 0xFF
 *   写按 16 字节，地址 16 对齐；写之前这 16 字节必须是 0xFF（真芯片每 16 字节带 ECC，不能写两次）
 * 模拟断电：ff_power_budget = N 表示还能完成 N 次操作（擦一个扇区算 1 次，写 16 字节算 1 次），
 *   用完后的操作都不执行、返回失败（断电了）。ff_power_on() 相当于重新上电。
 *   这里假设断电只发生在两次操作之间；"写 16 字节写到一半"的情况真芯片读出来会 ECC 错误（第 7a 步笔记第 6 节）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <stdint.h>
#include <string.h>
#include "flash_io.h"

#define FF_SECTOR   0x2000u
#ifndef FF_MAX
#define FF_MAX      0x10000u            /* 默认 64 KB；测试可以在 include 之前改 */
#endif

static uint8_t  ff_mem[FF_MAX];
static uint32_t ff_base;                /* ff_mem[0] 对应的地址 */
static uint32_t ff_size;
static long     ff_power_budget = -1;   /* -1 = 不限 */
static unsigned ff_erases, ff_writes, ff_double_write, ff_bad_args;

static inline void ff_init(uint32_t base, uint32_t size)
{
    ff_base = base;
    ff_size = size;
    memset(ff_mem, 0xFF, sizeof(ff_mem));
    ff_power_budget = -1;
    ff_erases = ff_writes = ff_double_write = ff_bad_args = 0;
}

static inline void ff_power_on(void) { ff_power_budget = -1; }

static inline int ff_use_power(void)
{
    if (ff_power_budget == 0)
        return 0;
    if (ff_power_budget > 0)
        ff_power_budget--;
    return 1;
}

static inline int ff_erase(uint32_t addr)
{
    if (addr < ff_base || addr - ff_base >= ff_size || (addr - ff_base) % FF_SECTOR != 0) {
        ff_bad_args++;
        return -1;
    }
    if (!ff_use_power())
        return -1;
    memset(&ff_mem[addr - ff_base], 0xFF, FF_SECTOR);
    ff_erases++;
    return 0;
}

static inline int ff_write(uint32_t addr, const uint8_t *d, uint32_t len)
{
    uint32_t off;

    if (addr < ff_base || addr - ff_base + len > ff_size || (addr & 15u) != 0 || (len & 15u) != 0) {
        ff_bad_args++;
        return -1;
    }
    for (off = 0; off < len; off += 16) {
        uint8_t *p = &ff_mem[addr - ff_base + off];
        int i;
        for (i = 0; i < 16; i++)
            if (p[i] != 0xFF) {
                ff_double_write++;
                return -2;
            }
        if (!ff_use_power())
            return -1;
        memcpy(p, &d[off], 16);
        ff_writes++;
    }
    return 0;
}

static inline const uint8_t *ff_ptr(uint32_t addr) { return &ff_mem[addr - ff_base]; }

static const flash_io_t ff_io = { ff_erase, ff_write, ff_ptr };

#endif /* FAKE_FLASH_H */
