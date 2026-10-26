/*
 * boot_guard.h - 防"App 一启动就死机"：启动次数计数、复位原因（Bootloader 和 App 共用，纯逻辑）
 *
 * 启动次数存在 TAMP 备份寄存器 0（复位不清，断电清；见 guard_hw.h）：
 *   Bootloader 每次跳 App 之前 +1，并打开看门狗
 *   App 稳定运行 5 秒（所有任务都按时报到）后清零 —— 叫做"确认"
 *   App 一启动就死机 → 看门狗复位 → 次数没清 → 再 +1 …… 到 3 次，Bootloader 不再跳，留下等升级
 *
 * 寄存器里的值 = 魔数 0xB007 << 16 | 次数。魔数不对（刚上电，备份寄存器是 0）就当 0 次。
 *
 * 测试标志存在备份寄存器 1：BG_TEST_CRASH_AT_BOOT = 下次开机 App 故意不喂狗、不确认（模拟"一启动就死机"）。
 *
 * 复位原因来自 RCC_RSR：一次复位可能同时置好几个标志（比如上电时 BOR 和 PIN 都是 1），按优先级取一个。
 *
 * PC 上测试：tests/test_boot_guard.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stdint.h>

#define BG_MAX_TRIES            3u
#define BG_TEST_CRASH_AT_BOOT   0x0001u

uint32_t bg_tries(uint32_t reg);            /* 备份寄存器 0 → 次数 */
uint32_t bg_tries_encode(uint32_t tries);   /* 次数 → 要写进去的值 */
uint32_t bg_flags(uint32_t reg);            /* 备份寄存器 1 → 测试标志 */
uint32_t bg_flags_encode(uint32_t flags);

/* 该跳的时候：次数已经到上限 → 1（留下），否则 0 */
int bg_too_many(uint32_t tries);

/* 复位原因（系统区 0xF006） */
typedef enum {
    RR_UNKNOWN = 0,
    RR_POWER_ON,        /* 上电 / 掉电复位（BOR） */
    RR_PIN,             /* 按了复位键（只有 PIN 标志） */
    RR_SOFTWARE,        /* NVIC_SystemReset：命令重启、升级、进出 Bootloader */
    RR_WATCHDOG         /* 独立看门狗 */
} rr_t;

rr_t rr_decode(uint32_t rcc_rsr);
const char *rr_name(rr_t r);

#endif /* BOOT_GUARD_H */
