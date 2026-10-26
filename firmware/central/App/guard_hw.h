/*
 * guard_hw.h - 备份寄存器和独立看门狗（IWDG）的寄存器操作，Bootloader 和 App 共用
 *
 * 备份寄存器（TAMP_BKPxR）：复位不清，断电清（板子 VBAT 没接电池）。写之前要：
 *   打开 RTC/TAMP 的 APB 时钟（RCC_APB3ENR.RTCAPBEN），再允许写备份域（PWR_DBPCR.DBP）
 *
 * IWDG：时钟是 32 kHz 的 LSI，分频 64 → 500 Hz，重装值 2000 → 4 秒不喂就复位。
 *   一旦打开，软件关不掉（只有复位能关）。选项字节 IWDG_SW = 1（10/26 已确认）：软件打开，不是上电自动开。
 *   调试时 CPU 停住，看门狗也跟着停（DBGMCU_APB1FZR1.DBG_IWDG_STOP），否则单步一会儿就被复位。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef GUARD_HW_H
#define GUARD_HW_H

#include "stm32h5xx.h"

#define BKP_REG_TRIES   0u      /* 启动次数 */
#define BKP_REG_FLAGS   1u      /* 测试标志 */

#define IWDG_RELOAD     2000u   /* 500 Hz × 4 s */

static inline void bkp_open(void)
{
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;
    (void)RCC->APB3ENR;
    PWR->DBPCR |= PWR_DBPCR_DBP;
}

static inline void bkp_close(void)              /* 恢复成复位后的样子（现在没用：见 bkp_write_check） */
{
    PWR->DBPCR &= ~PWR_DBPCR_DBP;
    RCC->APB3ENR &= ~RCC_APB3ENR_RTCAPBEN;
}

static inline uint32_t bkp_read(uint32_t i)
{
    return (&TAMP->BKP0R)[i];
}

static inline void bkp_write(uint32_t i, uint32_t v)
{
    (&TAMP->BKP0R)[i] = v;
}

/* 写完读回来，直到读到写进去的值（最多等一会儿）。成功 1。
 * 10/26 上板发现：Bootloader 写完马上 bkp_close()（关 DBP、关时钟），写进去的值丢了——
 * 写操作还没真正落到备份域，写保护就又打开了。读回来确认以后再做别的。 */
static inline int bkp_write_check(uint32_t i, uint32_t v)
{
    uint32_t n = 0;

    bkp_write(i, v);
    while (bkp_read(i) != v) {
        if (++n >= 100000u)
            return 0;
        bkp_write(i, v);
    }
    return 1;
}

static inline void iwdg_start(void)
{
    uint32_t n = 0;

    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;
    IWDG->KR  = 0xCCCCu;                        /* 打开（同时打开 LSI） */
    IWDG->KR  = 0x5555u;                        /* 允许改 PR、RLR */
    IWDG->PR  = 4u;                             /* /64 */
    IWDG->RLR = IWDG_RELOAD;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0u && ++n < 1000000u) { }
    IWDG->KR  = 0xAAAAu;                        /* 喂一次 */
}

static inline void iwdg_feed(void)
{
    IWDG->KR = 0xAAAAu;
}

#endif /* GUARD_HW_H */
