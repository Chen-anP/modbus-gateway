/*
 * tusb_config.h - TinyUSB 的配置（TinyUSB 编译时会 #include 这个文件，名字不能改）
 *
 * 只开设备端、一个 CDC（虚拟串口），跑在 FreeRTOS 上。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_STM32H5     /* 选 STM32 全速设备控制器驱动（stm32_fsdev） */
#define CFG_TUSB_OS             OPT_OS_FREERTOS     /* 内部的队列、互斥锁用 FreeRTOS 的 */
#define CFG_TUSB_DEBUG          0

#define CFG_TUD_ENABLED         1                   /* 设备端 */
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED /* H563 的 USB 只有全速 12 Mbit/s */
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

/* 用到的类：只有一个 CDC */
#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_NOTIFY      1                   /* CDC 规范要求的通知端点，Windows 驱动需要它 */
#define CFG_TUD_CDC_EP_BUFSIZE  64                  /* 全速批量端点最大包 64 字节 */
#define CFG_TUD_CDC_RX_BUFSIZE  512                 /* 收发 FIFO：Modbus 一帧最长 256 字节，留一倍 */
#define CFG_TUD_CDC_TX_BUFSIZE  512

#endif /* TUSB_CONFIG_H */
