/*
 * main.h - Bootloader 的公共头文件（共用的 flash_if.c 会 #include "main.h"）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef MAIN_H
#define MAIN_H

#include "stm32h5xx_hal.h"

extern UART_HandleTypeDef huart3;      /* 日志串口 */

void Error_Handler(void);

#endif /* MAIN_H */
