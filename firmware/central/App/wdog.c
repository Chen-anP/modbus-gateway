/*
 * wdog.c - App 的看门狗任务，说明见 wdog.h
 *
 * 为什么不在每个任务里直接喂狗：只要有一个任务还活着就会一直喂，别的任务卡死了也发现不了。
 * 改成"每个任务报到，一个专门的任务检查大家都报到了才喂"（wd.c），任何一个卡死都会触发复位。
 *
 * 看门狗任务的优先级比被监视的任务高：被监视的任务占着 CPU 不放（死循环）时，它照样能跑，
 * 能在日志里说出是谁卡住了——但只要有人没报到，它就不喂。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "log.h"
#include "guard_hw.h"
#include "boot_guard.h"
#include "wd.h"
#include "gw_data.h"
#include "wdog.h"

#define CHECK_MS        500u
#define STUCK_MS        3000u   /* 某个任务这么久没报到就不喂（看门狗 4 秒，留 1 秒余量） */
#define CONFIRM_MS      5000u   /* 开机后都正常这么久 → 确认 */
#define WDOG_STACK      256u
#define WDOG_PRIORITY   (tskIDLE_PRIORITY + 6)

static wd_t              s_wd;
static volatile uint8_t  s_hang_ch1;
static uint8_t           s_crash_test;      /* 本次开机是"开机就死机"测试 */

static const char *const k_names[WDOG_COUNT] = { "ch1", "ch2", "up" };

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void wdog_task(void *arg)
{
    uint32_t stuck = 0;
    uint8_t confirmed = 0, reported = 0;
    (void)arg;

    wd_init(&s_wd, WDOG_COUNT, STUCK_MS, now_ms());
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        if (s_crash_test) {                         /* 模拟"开机就死机"：不喂、不确认 */
            if (!reported) {
                LOGW("wdog", "test: crash at boot, not feeding");
                reported = 1;
            }
            continue;
        }
        if (wd_check(&s_wd, now_ms(), &stuck)) {
            iwdg_feed();
            reported = 0;
            if (!confirmed && now_ms() >= CONFIRM_MS) {
                bkp_open();
                (void)bkp_write_check(BKP_REG_TRIES, bg_tries_encode(0));
                confirmed = 1;
                LOGI("wdog", "boot confirmed (all tasks alive for %lu ms), heap free %lu / min %lu bytes",
                     (unsigned long)CONFIRM_MS, (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize());
            }
        } else if (!reported) {
            LOGE("wdog", "task %s stuck for > %lu ms, stop feeding: reset in a few seconds",
                 k_names[stuck], (unsigned long)STUCK_MS);
            reported = 1;
        }
    }
}

void wdog_start(void)
{
    uint32_t rsr = RCC->RSR, tries, flags;
    rr_t rr = rr_decode(rsr);
    BaseType_t r;

    RCC->RSR |= RCC_RSR_RMVF;                       /* 清掉标志，下次复位重新记 */
    bkp_open();
    tries = bg_tries(bkp_read(BKP_REG_TRIES));
    flags = bg_flags(bkp_read(BKP_REG_FLAGS));
    s_crash_test = (flags & BG_TEST_CRASH_AT_BOOT) ? 1u : 0u;

    gw_sys()->reset_reason = (uint16_t)rr;          /* 调度器还没启动，不用加锁 */
    gw_sys()->boot_tries   = (uint16_t)tries;
    LOGI("wdog", "reset reason: %s (RSR 0x%08lX), boot try %lu/%u", rr_name(rr), (unsigned long)rsr,
         (unsigned long)tries, (unsigned)BG_MAX_TRIES);

    iwdg_start();                                   /* Bootloader 已经开过；再开一次是为了用 Keil 单独下载 App 时也有 */
    r = xTaskCreate(wdog_task, "wdog", WDOG_STACK, NULL, WDOG_PRIORITY, NULL);
    configASSERT(r == pdPASS);
    (void)r;
}

void wdog_beat(uint32_t id)
{
    wd_beat(&s_wd, id);
}

void wdog_allow(uint32_t id, uint32_t ms)
{
    wd_allow(&s_wd, id, now_ms(), ms);
}

void wdog_test_hang_ch1(void)
{
    s_hang_ch1 = 1;
}

int wdog_test_should_hang(uint32_t id)
{
    return id == WDOG_CH1 && s_hang_ch1;
}

void wdog_test_crash_at_boot(void)
{
    bkp_open();
    (void)bkp_write_check(BKP_REG_FLAGS, bg_flags_encode(BG_TEST_CRASH_AT_BOOT));
}
