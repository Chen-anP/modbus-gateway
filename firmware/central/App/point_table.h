/*
 * point_table.h - 点表：中控寄存器空间里的每个地址，对应哪个通道上哪个传感器的哪个寄存器
 *
 * 一个"点"：中控的 <表, 中控地址>  ⇄  <通道, 设备地址, 同一张表, 传感器寄存器地址>
 * 例：中控 1x 0 = ch1 上地址 1 的设备的 1x 0（开关量板的按键 1）
 *
 * 点表文件格式（设计文档 4.3 节，每个点 8 字节，大端）：
 *
 *   字节   0     1      2         3      4～5         6～7
 *   内容   表    通道   设备地址  保留   中控地址      传感器寄存器地址
 *          0=0x 1=1x 2=3x 3=4x（和 mb_table_t 一样）
 *
 * 校验（任何一条不满足，整张表拒绝，旧表继续生效）：
 *   长度是 8 的倍数、点数 1～PT_MAX_POINTS；表 0～3；通道 1～2；设备地址 1～247；
 *   中控地址 ≤ 499；同一个"表 + 中控地址"不能出现两次；每个通道最多 8 个设备
 *
 * 解析后按 <通道, 设备, 表, 传感器地址> 排序，再把"地址连续"的点合并成读块：
 * 一个读块 = 一次 Modbus 读请求。比如开关量板的 5 个线圈合成一次"读线圈 0～4"。
 *
 * 纯逻辑，PC 上测试：tests/test_point_table.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef POINT_TABLE_H
#define POINT_TABLE_H

#include <stdint.h>
#include "mb_rtu.h"

#define PT_MAX_POINTS   128u    /* 最多多少个点（RAM：每点约 20 字节） */
#define PT_GW_ADDR_MAX  499u    /* 中控每张表的地址 0～499 */
#define PT_REC_SIZE     8u      /* 文件里每个点 8 字节 */
#define PT_BLOCK_MAX    16u     /* 一个读块最多几个点 */
#define PT_CH_COUNT     2u
#define PT_MAX_DEVS     8u      /* 每个通道最多几个设备（point_sync 为每个设备记在线状态） */

typedef struct {
    uint8_t  tab;           /* mb_table_t */
    uint8_t  ch;            /* 1、2 */
    uint8_t  dev;           /* 传感器地址 */
    uint16_t gw_addr;       /* 中控地址 */
    uint16_t dev_addr;      /* 传感器寄存器地址 */
} pt_point_t;

typedef struct {
    uint8_t  ch, dev, tab;
    uint16_t start;         /* 传感器起始地址 */
    uint16_t count;
    uint16_t first;         /* 第一个点在 pts[] 里的下标；这个块是 pts[first .. first+count) */
} pt_block_t;

typedef enum {
    PT_OK = 0,
    PT_ERR_LEN,             /* 长度不是 8 的倍数，或者是 0 */
    PT_ERR_COUNT,           /* 点太多 */
    PT_ERR_TAB,
    PT_ERR_CH,
    PT_ERR_DEV,
    PT_ERR_ADDR,            /* 中控地址超过 499 */
    PT_ERR_DUP,             /* 同一个"表 + 中控地址"出现两次 */
    PT_ERR_DEVS             /* 某个通道上的设备超过 PT_MAX_DEVS 个 */
} pt_err_t;

typedef struct {
    pt_point_t pts[PT_MAX_POINTS];      /* 已排序 */
    uint16_t   n;
    pt_block_t blocks[PT_MAX_POINTS];
    uint16_t   nblocks;
} point_table_t;

/* 解析并校验。成功返回 PT_OK；失败返回错误码，*bad 是出错的点的序号（文件里第几个，从 0 开始）。
 * 失败时 t 的内容不可用：调用者应该先解析到一张临时表，成功了再替换正在用的表 */
pt_err_t point_table_parse(point_table_t *t, const uint8_t *buf, uint32_t len, uint16_t *bad);

/* 把点编码成文件格式（给内置默认表和测试用）。返回写入的字节数，空间不够返回 0 */
uint32_t point_table_encode(const pt_point_t *pts, uint16_t n, uint8_t *buf, uint32_t size);

/* 按中控的 <表, 地址> 找点，返回下标；找不到返回 -1 */
int point_table_find(const point_table_t *t, mb_table_t tab, uint16_t gw_addr);

const char *pt_err_name(pt_err_t e);

#endif /* POINT_TABLE_H */
