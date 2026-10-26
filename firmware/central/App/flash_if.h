/*
 * flash_if.h - 板子上的 Flash 擦写（HAL），实现 flash_io_t
 *
 * 安全限制：只允许擦写"窗口"里的地址。App 里窗口是点表区 + 配置区（0x081FC000～0x081FFFFF），
 * 写错地址（比如把 App 自己擦了）会直接返回失败。第 7b 步 Bootloader 会把窗口设成 App 区 + 配置区。
 *
 * 不能在中断里调用；擦一个扇区要几毫秒，调用它的任务这段时间不返回（别的任务照常运行，
 * 因为程序在 Bank 1 跑、擦的是 Bank 2，CPU 取指令不用等）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdint.h>
#include "flash_io.h"

/* 允许擦写的地址范围 [start, end)，必须按扇区对齐 */
void flash_if_set_window(uint32_t start, uint32_t end);

/* 给 pt_store、boot_cfg 用的一组函数 */
const flash_io_t *flash_if_io(void);

typedef struct {
    uint32_t erases, writes;    /* 成功的次数（扇区 / 16 字节） */
    uint32_t refused;           /* 地址在窗口外、没对齐 */
    uint32_t hal_errors;        /* HAL 返回失败 */
    uint32_t last_hal_error;    /* HAL_FLASH_GetError() 的值 */
    uint32_t last_erase_ms;     /* 最近一次擦扇区用了多久 */
} flash_if_stats_t;

const flash_if_stats_t *flash_if_stats(void);

/* 把选项字节的关键几项打到日志里（开机时调用，第 7 步前已在 Keil 里确认过） */
void flash_if_log_option_bytes(void);

#endif /* FLASH_IF_H */
