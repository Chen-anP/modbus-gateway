/*
 * dev_health.h - 设备在线状态：连续失败几次算掉线，掉线后多久再试一次
 *
 *            成功                      连续失败 3 次
 *  UNKNOWN ────────▶ ONLINE ──────────────────────────▶ OFFLINE
 *     │                ▲                                   │
 *     │ 连续失败 3 次    └────────────── 成功 ───────────────┘
 *     └──────────────────────────────────▶ OFFLINE         掉线期间每 2 s 才试一次
 *
 * 为什么不是"失败一次就算掉线"：传感器刚上电还没准备好、总线上偶尔一个干扰，都会让单次请求失败（第 2 步实测：
 * 环境监测板开机后第一次请求超时）。连续 3 次才认定掉线，偶发失败不会让状态来回跳。
 * 为什么掉线后要降频：每次请求要等满超时（100 ms × 重试），一个掉线设备如果每轮都试，会拖慢同一通道上的所有设备。
 *
 * 纯逻辑，不依赖 RTOS，PC 上测试：tests/test_dev_health.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef DEV_HEALTH_H
#define DEV_HEALTH_H

#include <stdint.h>

#define DEV_OFFLINE_AFTER       3u      /* 连续失败几次判定掉线 */
#define DEV_OFFLINE_RETRY_MS    2000u   /* 掉线后多久再试一次 */

typedef enum {
    DEV_UNKNOWN = 0,    /* 上电后还没成功过 */
    DEV_ONLINE,
    DEV_OFFLINE
} dev_state_t;

typedef struct {
    dev_state_t state;
    uint8_t     fails;          /* 当前连续失败次数 */
    uint32_t    retry_at_ms;    /* 掉线时：这个时刻之后才再试 */
    uint32_t    ok_count;       /* 统计：成功次数 */
    uint32_t    fail_count;     /* 统计：失败次数 */
    uint32_t    offline_count;  /* 统计：掉线了几次 */
} dev_health_t;

void dev_health_init(dev_health_t *h);

/* 现在该不该去问这个设备：在线 / 未知时总是该问；掉线时只有到了 retry_at_ms 才问 */
int dev_health_due(const dev_health_t *h, uint32_t now_ms);

/* 报告一次结果。alive = 1 表示设备回应了（正常回应或异常码都算）。
 * 返回状态变化：+1 变成在线，-1 变成掉线，0 没变 */
int dev_health_report(dev_health_t *h, int alive, uint32_t now_ms);

const char *dev_state_name(dev_state_t s);

#endif /* DEV_HEALTH_H */
