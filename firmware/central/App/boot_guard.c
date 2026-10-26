/*
 * boot_guard.c - 启动次数和复位原因，说明见 boot_guard.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "boot_guard.h"

#define TRIES_MAGIC 0xB0070000u
#define FLAGS_MAGIC 0x7E570000u

/* RCC_RSR 的几个标志位（和 stm32h563xx.h 里的 RCC_RSR_xxxRSTF 一样，这里写死，PC 测试不用芯片头文件） */
#define RSR_PIN     (1u << 26)
#define RSR_BOR     (1u << 27)
#define RSR_SFT     (1u << 28)
#define RSR_IWDG    (1u << 29)

uint32_t bg_tries(uint32_t reg)
{
    return ((reg & 0xFFFF0000u) == TRIES_MAGIC) ? (reg & 0xFFu) : 0u;
}

uint32_t bg_tries_encode(uint32_t tries)
{
    return TRIES_MAGIC | (tries > 0xFFu ? 0xFFu : tries);
}

uint32_t bg_flags(uint32_t reg)
{
    return ((reg & 0xFFFF0000u) == FLAGS_MAGIC) ? (reg & 0xFFFFu) : 0u;
}

uint32_t bg_flags_encode(uint32_t flags)
{
    return FLAGS_MAGIC | (flags & 0xFFFFu);
}

int bg_too_many(uint32_t tries)
{
    return tries >= BG_MAX_TRIES;
}

/* 内部复位（软件、看门狗）也会让复位脚拉低，PIN 标志跟着置位；所以先看别的，最后才看 PIN */
rr_t rr_decode(uint32_t rsr)
{
    if (rsr & RSR_IWDG) return RR_WATCHDOG;
    if (rsr & RSR_SFT)  return RR_SOFTWARE;
    if (rsr & RSR_BOR)  return RR_POWER_ON;
    if (rsr & RSR_PIN)  return RR_PIN;
    return RR_UNKNOWN;
}

const char *rr_name(rr_t r)
{
    static const char *const k[] = { "unknown", "power on", "reset pin", "software", "watchdog" };
    return ((unsigned)r < sizeof(k) / sizeof(k[0])) ? k[r] : "?";
}
