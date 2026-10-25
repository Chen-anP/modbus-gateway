/*
 * poll_task.h - 第 3 步：两个通道各一个轮询任务
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef POLL_TASK_H
#define POLL_TASK_H

#include "poll.h"

/* 创建 ch1、ch2 两个轮询任务。在 MX_FREERTOS_Init 里调用（替换第 2 步的 bus_probe_start） */
void poll_start(void);

/* 给调试 / 以后的模块用：取某个通道的轮询状态（0 = ch1，1 = ch2），没有返回 NULL */
const poll_ch_t *poll_get(uint8_t ch);

#endif /* POLL_TASK_H */
