/*
 * tusb_config.h - Bootloader 的 TinyUSB 配置
 *
 * 和 App 的唯一区别：没有 RTOS（OPT_OS_NONE），主循环里不停地调用 tud_task()。
 * Keil 工程的头文件路径里 boot/Core/Inc 排在前面，所以 TinyUSB 用的是这一份。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_STM32H5
#define CFG_TUSB_OS             OPT_OS_NONE         /* 裸机：需要提供 tusb_time_millis_api()（boot_usb.c） */
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_NOTIFY      1
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  512

#endif /* TUSB_CONFIG_H */
