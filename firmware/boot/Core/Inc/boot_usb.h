/*
 * boot_usb.h - Bootloader 的 USB 虚拟串口（裸机版 TinyUSB）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BOOT_USB_H
#define BOOT_USB_H

#include <stdint.h>

int      boot_usb_init(void);                                       /* 时钟、TinyUSB。成功 0 */
void     boot_usb_poll(void);                                       /* 主循环里一直调用 */
uint16_t boot_usb_read(uint8_t *buf, uint16_t max);                 /* 不等待，没有数据返回 0 */
int      boot_usb_write(const uint8_t *d, uint16_t len, uint32_t timeout_ms);
void     boot_usb_irq(void);                                        /* USB 中断里调用 */
void     boot_wait_ms(uint32_t ms);                                 /* 等待，期间照常处理 USB */

#endif /* BOOT_USB_H */
