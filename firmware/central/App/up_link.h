/*
 * up_link.h - 上行：电脑通过 USB 虚拟串口访问中控（中控是 Modbus 从机，地址 1）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef UP_LINK_H
#define UP_LINK_H

#define UP_SLAVE_ADDR   1u      /* 中控在上行链路上的从机地址 */

/* 初始化 USB（创建 usbd 任务）并创建 up_link 任务。在 MX_FREERTOS_Init 里调用 */
void up_link_start(void);

#endif /* UP_LINK_H */
