/*
 * bus_probe.h - 第 2 步的测试任务（第 3 步删除）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BUS_PROBE_H
#define BUS_PROBE_H

/* 创建测试任务：每秒读一次两路 RS485 上的传感器。在 MX_FREERTOS_Init 里调用 */
void bus_probe_start(void);

#endif
