/*
 * poll_task.h - ch1、ch2 两个通道任务：按点表同步传感器（第 3 步是设备表，第 5 步换成点表）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef POLL_TASK_H
#define POLL_TASK_H

#include "point_sync.h"

/* 加载点表（gw_data_init）并创建 ch1、ch2 两个任务。在 MX_FREERTOS_Init 里调用 */
void poll_start(void);

/* 取某个通道的同步状态（0 = ch1，1 = ch2），任务还没起来返回 NULL */
const point_sync_t *poll_get(uint8_t ch);

#endif /* POLL_TASK_H */
