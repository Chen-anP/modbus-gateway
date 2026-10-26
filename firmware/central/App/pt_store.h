/*
 * pt_store.h - 把下发的点表存进 Flash（点表区 8 KB），重启后还在
 *
 * 扇区里的格式：
 *   +0   头 32 字节：魔数 'GWPT'、格式版本、数据长度、数据的 CRC32、保存次数、保留、头自己的 CRC32
 *   +32  点表文件（就是上位机发来的那个文件，8 字节一个点）
 *
 * 写的顺序：擦扇区 → 写数据 → **最后写头**。
 *   写数据时断电：头还是擦除后的 0xFF，魔数对不上 → 当作"没有保存过"，用内置默认表
 *   写头时断电：头的 CRC 对不上 → 同上
 * 所以任何时候断电，读出来的要么是完整的新表，要么是"没有"，不会是半张表。
 * （代价：断电那一次旧表也没了，回到默认表。要保住旧表就得用两个扇区轮流写——第 8 节有讨论。）
 *
 * 纯逻辑，PC 上测试：tests/test_pt_store.c（含模拟断电）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef PT_STORE_H
#define PT_STORE_H

#include <stdint.h>
#include "flash_io.h"

#define PT_STORE_MAGIC      0x47575054u     /* 'GWPT' */
#define PT_STORE_HDR_SIZE   32u
#define PT_STORE_MAX_DATA   1024u           /* 128 个点 × 8 字节 */

typedef enum {
    PTS_OK = 0,
    PTS_EMPTY,          /* 没有保存过（或者头坏了）：用默认表 */
    PTS_BAD_DATA,       /* 头是好的，数据 CRC 不对：用默认表，记一条警告 */
    PTS_TOO_BIG,
    PTS_FLASH_ERR       /* 擦或写失败 */
} pts_result_t;

/* 读：成功时 *data 指向 Flash 里的点表文件，*len 是长度，*seq 是第几次保存 */
pts_result_t pt_store_load(const flash_io_t *io, uint32_t addr, const uint8_t **data, uint32_t *len, uint32_t *seq);

/* 存：擦扇区、写数据、最后写头。seq 是新的保存次数 */
pts_result_t pt_store_save(const flash_io_t *io, uint32_t addr, const uint8_t *data, uint32_t len, uint32_t seq);

/* 清掉（擦扇区）：下次开机用内置默认表 */
pts_result_t pt_store_clear(const flash_io_t *io, uint32_t addr);

const char *pts_name(pts_result_t r);

#endif /* PT_STORE_H */
