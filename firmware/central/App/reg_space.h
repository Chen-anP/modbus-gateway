/*
 * reg_space.h - 中控对上位机的寄存器空间：映射区（由点表决定）+ 系统区（sys_regs）
 *
 *   映射区   四张表各 0～499：点表里有的地址才能读写，其余回异常 02
 *   系统区   4x 0xF000～：交给 sys_regs
 *
 * 每个点有三种状态（设计文档第 5 节）：
 *
 *   未同步   上电后还没读到过    上位机读到 0；不会因此往设备写 0
 *   已同步   正常                 读到最近一次从设备读到的值
 *   待写入   上位机写了还没下发   读到上位机写的值；通道任务下发成功后清除
 *
 * 为什么用"待写入"标记，不比较新旧值（课程的做法）：上电时全是 0，比较会把 0 写到所有输出；
 * 读回来的值还可能覆盖掉上位机刚写的值。现在：读回的值只在"没有待写入"时才更新。
 *
 * 两个任务会用它：
 *   up 任务     收到上位机请求 → mb_slave_handle() → 这里的读写回调
 *   ch1 / ch2   取待写入的点下发、把读到的值更新进来
 * 所以要一把互斥锁：up 任务把整个 mb_slave_handle() 包在锁里（回调里不再加锁）；
 * ch 任务调用的 reg_space_next_pending / write_result / update 自己加锁。锁里不做任何收发。
 *
 * 纯逻辑，锁通过函数指针传进来（PC 测试里是空函数）：tests/test_reg_space.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef REG_SPACE_H
#define REG_SPACE_H

#include <stdint.h>
#include "mb_rtu.h"
#include "point_table.h"
#include "sys_regs.h"

#define RS_SYNCED   0x01u   /* 读到过 */
#define RS_PENDING  0x02u   /* 待写入 */

/* 下发结果 */
typedef enum {
    RS_WRITE_OK = 0,        /* 设备确认了 */
    RS_WRITE_RETRY,         /* 超时、CRC 错：下一轮再试 */
    RS_WRITE_REJECTED       /* 设备回了异常码：不再重试，记到 LAST_ERR */
} rs_write_result_t;

typedef struct {
    uint16_t value;         /* 最近一次从设备读到（或写成功）的值 */
    uint16_t pend_value;    /* 待写入的值 */
    uint8_t  flags;
} rs_point_t;

typedef struct {
    const point_table_t *pt;
    rs_point_t  st[PT_MAX_POINTS];                      /* 和 pt->pts[] 一一对应 */
    int16_t     map[MB_TAB_COUNT][PT_GW_ADDR_MAX + 1];  /* 中控地址 → 点下标，-1 = 没映射 */
    sys_regs_t *sys;
    void      (*lock)(void);
    void      (*unlock)(void);
    uint32_t    writes;     /* 统计：上位机写入了几个点 */
} reg_space_t;

/* lock / unlock 可以是 NULL（PC 测试、单任务时） */
void reg_space_init(reg_space_t *rs, sys_regs_t *sys, void (*lock)(void), void (*unlock)(void));

/* 换上一张点表：所有点回到"未同步"，待写入全部清掉，POINT_CNT 更新。内部加锁 */
void reg_space_load(reg_space_t *rs, const point_table_t *pt);

/* 从机回调（不加锁，调用 mb_slave_handle 的地方负责加锁） */
void reg_space_ops(reg_space_t *rs, mb_slave_ops_t *ops);

/* ---- 通道任务用（内部加锁） ---- */

/* 从下标 from 开始，找通道 ch 上下一个待写入的点。找到返回下标，*value 是要写的值；没有返回 -1 */
int  reg_space_next_pending(reg_space_t *rs, uint8_t ch, uint16_t from, uint16_t *value);

/* 报告下发结果。written 是当时写下去的值：如果这期间上位机又写了别的值，标记保留，下一轮再发新值 */
void reg_space_write_result(reg_space_t *rs, uint16_t idx, uint16_t written, rs_write_result_t r);

/* 读到设备的值。待写入期间不更新（不能覆盖上位机刚写的值）。返回 1 = 上位机看到的值变了 */
int  reg_space_update(reg_space_t *rs, uint16_t idx, uint16_t value);

/* 上位机现在读这个点会读到什么（测试、日志用） */
uint16_t reg_space_get(reg_space_t *rs, uint16_t idx);

#endif /* REG_SPACE_H */
