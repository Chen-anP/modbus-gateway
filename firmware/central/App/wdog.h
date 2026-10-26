/*
 * wdog.h - App 的看门狗任务：所有被监视的任务都按时报到才喂狗；稳定 5 秒后"确认"本次启动
 *
 *   被监视的任务：ch1、ch2（每一轮）、up（每次循环，最多 1 秒一次）
 *   某个任务超过 3 秒没报到 → 停止喂狗 → 最多 4 秒后看门狗复位（日志里会说是哪个任务）
 *   开机后所有任务都正常跑满 5 秒 → 把备份寄存器里的启动次数清零（Bootloader 那边见 boot_guard.h）
 *
 * 故障测试命令（系统区 CMD，第 8 步）：
 *   0x0F01  ch1 任务故意卡住（模拟死锁），看门狗应该在几秒后复位中控
 *   0x0F02  设置"开机就死机"测试标志并重启：之后每次开机都不喂狗、不确认，连续 3 次后留在 Bootloader
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef WDOG_H
#define WDOG_H

#include <stdint.h>

enum { WDOG_CH1 = 0, WDOG_CH2, WDOG_UP, WDOG_COUNT };

/* 调度器启动前调用：读复位原因、启动次数，写进系统区；打开看门狗；创建看门狗任务 */
void wdog_start(void);

/* 被监视的任务每一轮调用 */
void wdog_beat(uint32_t id);

/* 要做一件可能很久的事（转发固件时等传感器擦 Flash）：接下来 ms 毫秒里不报到也不算卡住 */
void wdog_allow(uint32_t id, uint32_t ms);

/* 故障测试 */
void wdog_test_hang_ch1(void);           /* 只是设一个标志，ch1 任务下一轮自己卡住 */
int  wdog_test_should_hang(uint32_t id); /* ch1 任务每轮问一下 */
void wdog_test_crash_at_boot(void);      /* 设备份寄存器标志（调用者负责重启） */

#endif /* WDOG_H */
