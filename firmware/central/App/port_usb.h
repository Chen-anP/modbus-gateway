/*
 * port_usb.h - USB 虚拟串口（CDC）收发层，用 TinyUSB
 *
 * 和 port_uart 用法一样：一个任务读、一个任务写；读有超时，写等发完或超时。
 *
 *   port_usb_init();                                // 创建 usbd 任务（TinyUSB 在里面初始化和处理 USB 事件）
 *   n = port_usb_read(buf, sizeof(buf), 50);        // 读，最多等 50 ms
 *   port_usb_write(rsp, len, 100);                  // 写
 *
 * 数据流：
 *   电脑 → USB 中断 → TinyUSB 把事件放进队列 → usbd 任务 tud_task() 处理
 *        → 数据进 CDC 的 RX FIFO → tud_cdc_rx_cb() 给信号量 → 等在 port_usb_read 里的任务醒来取数据
 *   回应 → port_usb_write → CDC 的 TX FIFO → tud_cdc_write_flush() 启动 IN 传输 → 电脑取走
 *
 * USB 中断：CubeMX 生成的 USB_DRD_FS_IRQHandler 里，USER CODE 区调用 port_usb_irq()。
 * TinyUSB 初始化之前中断还交给 HAL 处理，初始化之后交给 TinyUSB。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef PORT_USB_H
#define PORT_USB_H

#include <stdint.h>

typedef struct {
    volatile uint32_t rx_bytes;
    volatile uint32_t tx_bytes;
    volatile uint32_t tx_timeouts;      /* 写超时（电脑没在读、或者没连接） */
    volatile uint32_t mounts;           /* 被电脑枚举成功的次数 */
    volatile uint8_t  mounted;          /* 现在是否已枚举 */
    volatile uint8_t  dtr;              /* 电脑端串口是否打开（DTR 信号） */
} port_usb_stats_t;

/* 创建 usbd 任务。在 MX_FREERTOS_Init 里（调度器启动前）调用。成功返回 0 */
int port_usb_init(void);

/* 有数据立刻返回（最多 max 字节）；没有数据最多等 timeout_ms。返回读到的字节数，0 = 超时 */
uint16_t port_usb_read(uint8_t *buf, uint16_t max, uint32_t timeout_ms);

/* 写入并发出去。全部写进发送 FIFO 返回 0；超时（没连接、电脑不读）返回 -1 */
int port_usb_write(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* 丢弃还没读走的数据 */
void port_usb_flush(void);

/* USB 中断里调用：TinyUSB 已初始化就交给它处理并返回 1；还没初始化返回 0（由 HAL 处理） */
int port_usb_irq(void);

const port_usb_stats_t *port_usb_stats(void);

#endif /* PORT_USB_H */
