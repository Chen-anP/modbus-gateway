/*
 * point_sync.h - 点同步：一个通道上，按点表把"待写入"的点下发、把所有点读回来
 *
 * 每个通道任务循环调用 point_sync_step()，一轮做两件事（设计文档第 5 节）：
 *   1. 下发：取本通道所有待写入的点，逐个写到传感器（线圈 05、保持寄存器 06）
 *            成功 → 清标记；超时 → 保留，下一轮再试；设备回异常码 → 放弃，记 LAST_ERR
 *   2. 读取：按读块（点表里地址连续的点合并成一次读）逐个读，成功就更新寄存器空间
 * 设备的在线状态沿用第 3 步的 dev_health：连续失败 3 次掉线，掉线后每 2 s 只试 1 次。
 *
 * 为什么先写后读：上位机写了一个输出，希望尽快生效；而且写完马上读回来，上位机能看到设备的真实状态。
 *
 * 纯逻辑：收发走 mb_master（mb_io_t），PC 上用模拟从机测试：tests/test_point_sync.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef POINT_SYNC_H
#define POINT_SYNC_H

#include <stdint.h>
#include "mb_master.h"
#include "dev_health.h"
#include "point_table.h"
#include "reg_space.h"

#define PS_MAX_DEVS     PT_MAX_DEVS     /* 一个通道上最多几个设备（点表解析时已经检查过） */

typedef struct {
    uint8_t      addr;
    dev_health_t health;
} ps_dev_t;

typedef struct {
    uint8_t              ch;
    mb_master_t         *mb;
    reg_space_t         *rs;
    const point_table_t *pt;
    ps_dev_t             devs[PS_MAX_DEVS];
    uint8_t              ndev;
    int8_t               blk_rc[PT_MAX_POINTS];     /* 每个读块上一次的结果，只在变化时打日志 */
    /* 统计 */
    uint32_t rounds, reads_ok, writes_ok, writes_retry, writes_rejected;
} point_sync_t;

/* 从点表里找出本通道的设备。设备太多返回 -1 */
int point_sync_init(point_sync_t *ps, uint8_t ch, mb_master_t *mb, reg_space_t *rs, const point_table_t *pt);

/* 一轮：先下发，再读 */
void point_sync_step(point_sync_t *ps);

/* 本通道现在掉线的设备数 */
uint8_t point_sync_offline(const point_sync_t *ps);

/* INFO 等级打印统计和每个设备的状态 */
void point_sync_report(const point_sync_t *ps);

#endif /* POINT_SYNC_H */
