/*
 * wd.h - 任务看门狗的判断逻辑（纯逻辑）
 *
 * 只要有一个任务活着就喂狗，看门狗只能发现"整个系统卡死"。这里改成：
 * 每个要监视的任务每一轮调用 wd_beat(id)（计数 + 1），看门狗任务每 0.5 秒调用 wd_check()：
 * 所有任务在最近 timeout_ms 内都报到过 → 返回 1，喂狗；有一个超时 → 返回 0，不喂，*stuck = 它的编号。
 *
 * 只有一个任务写 beat[id]、一个任务读，32 位读写在 Cortex-M 上是原子的，不用加锁。
 *
 * PC 上测试：tests/test_wd.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef WD_H
#define WD_H

#include <stdint.h>

#define WD_MAX_TASKS 4u

typedef struct {
    volatile uint32_t beat[WD_MAX_TASKS];   /* 各任务自己加 */
    uint32_t seen[WD_MAX_TASKS];            /* 上次检查时看到的值 */
    uint32_t last_ok[WD_MAX_TASKS];         /* 上次看到变化的时间 */
    volatile uint32_t allow_until[WD_MAX_TASKS];    /* 在这个时间之前不算卡住（任务事先声明要等很久） */
    uint32_t n;
    uint32_t timeout_ms;
} wd_t;

void wd_init(wd_t *w, uint32_t n, uint32_t timeout_ms, uint32_t now_ms);
void wd_beat(wd_t *w, uint32_t id);

/* 任务要做一件可能很久的事（比如转发固件时等传感器擦 Flash）：在 now_ms + ms 之前不算它卡住 */
void wd_allow(wd_t *w, uint32_t id, uint32_t now_ms, uint32_t ms);
int  wd_check(wd_t *w, uint32_t now_ms, uint32_t *stuck);

#endif /* WD_H */
