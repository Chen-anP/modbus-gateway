/*
 * poll.h - 主站轮询：一个通道上的设备按各自的周期去读，记录在线状态和最近一次的值
 *
 * 每个通道一个 poll_ch_t，由一个任务反复调用 poll_ch_step()：
 *   对每个设备：到了它的周期，而且（如果掉线）到了重试时间 → 读一次 → 更新值和在线状态
 *
 * 纯逻辑：收发通过 mb_master（再往下是 mb_io_t 函数指针），时间也从 mb_io_t 取，
 * 所以可以在 PC 上用模拟从机测试（tests/test_poll.c）。任务、串口、设备表在 poll_task.c。
 *
 * 第 5 步会把"设备表"换成"点表"（point_sync），这里的在线判断和请求层原样复用。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef POLL_H
#define POLL_H

#include <stdint.h>
#include "mb_rtu.h"
#include "mb_master.h"
#include "dev_health.h"

#define POLL_MAX_VALUES 8u      /* 每个设备一次最多读几个点 */

/* 设备配置（常量表） */
typedef struct {
    const char *name;           /* 日志用 */
    uint8_t     addr;           /* 从机地址 */
    mb_table_t  tab;            /* 读哪张表 */
    uint16_t    start;          /* 起始地址 */
    uint16_t    count;          /* 个数，≤ POLL_MAX_VALUES */
    uint32_t    period_ms;      /* 多久读一次 */
} poll_dev_cfg_t;

/* 设备运行状态 */
typedef struct {
    const poll_dev_cfg_t *cfg;
    dev_health_t health;
    uint32_t     next_ms;                   /* 下一次该读的时刻 */
    uint16_t     values[POLL_MAX_VALUES];   /* 最近一次读到的值 */
    uint8_t      have_values;               /* 读到过至少一次 */
    int          last_rc;                   /* 最近一次的结果（MB_OK / MB_ERR_xxx） */
    mb_ex_t      last_ex;                   /* 最近一次的异常码 */
} poll_dev_t;

typedef struct {
    mb_master_t *mb;
    poll_dev_t  *devs;
    uint8_t      ndev;
} poll_ch_t;

/* devs 由调用者提供（ndev 个），cfgs 是常量表。配置非法（个数超过 POLL_MAX_VALUES 等）返回 -1 */
int poll_ch_init(poll_ch_t *ch, mb_master_t *mb, poll_dev_t *devs, const poll_dev_cfg_t *cfgs, uint8_t ndev);

/* 扫一遍：所有"到期"的设备各读一次。返回这次实际读了几个设备 */
uint8_t poll_ch_step(poll_ch_t *ch);

/* 打印每个设备的状态、统计和最近的值（INFO 等级） */
void poll_ch_report(const poll_ch_t *ch);

/* 按地址找设备，找不到返回 NULL */
const poll_dev_t *poll_ch_find(const poll_ch_t *ch, uint8_t addr);

#endif /* POLL_H */
