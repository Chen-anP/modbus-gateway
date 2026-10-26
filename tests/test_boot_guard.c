/*
 * test_boot_guard.c - 启动次数、测试标志、复位原因
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/boot_guard.c tests/test_boot_guard.c
 *
 * 第 5 组模拟"App 一启动就死机"：Bootloader 跳 3 次以后不再跳。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "boot_guard.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    uint32_t reg, i;

    /* 1. 刚上电：备份寄存器是 0 → 0 次；随便一个值（魔数不对）→ 0 次 */
    CHECK(bg_tries(0) == 0);
    CHECK(bg_tries(0x12345603u) == 0);
    CHECK(bg_tries(0xFFFFFFFFu) == 0);

    /* 2. 编码、解码 */
    for (i = 0; i <= 5; i++)
        CHECK(bg_tries(bg_tries_encode(i)) == i);
    CHECK(bg_tries(bg_tries_encode(1000)) == 0xFF);        /* 不会溢出到魔数里 */

    /* 3. 上限 */
    CHECK(!bg_too_many(0) && !bg_too_many(2) && bg_too_many(3) && bg_too_many(4));

    /* 4. 测试标志：魔数不对 → 没有标志；次数寄存器的值当标志读 → 没有标志（两个魔数不一样） */
    CHECK(bg_flags(0) == 0);
    CHECK(bg_flags(bg_flags_encode(BG_TEST_CRASH_AT_BOOT)) == BG_TEST_CRASH_AT_BOOT);
    CHECK(bg_flags(bg_tries_encode(1)) == 0);
    CHECK(bg_tries(bg_flags_encode(1)) == 0);

    /* 5. 模拟：App 每次启动都死机（没机会清零），Bootloader 每次跳之前 +1 */
    reg = 0;
    {
        int jumps = 0;
        for (i = 0; i < 10; i++) {                         /* 10 次复位 */
            uint32_t t = bg_tries(reg);
            if (bg_too_many(t))
                break;                                     /* 留在 Bootloader */
            reg = bg_tries_encode(t + 1);                  /* 跳之前 +1 */
            jumps++;
            /* App 启动就死机：不清零，看门狗复位 */
        }
        CHECK(jumps == 3 && bg_tries(reg) == 3);
    }
    /* 再模拟：App 正常，启动 5 秒后清零 → 每次都能跳 */
    reg = 0;
    for (i = 0; i < 10; i++) {
        uint32_t t = bg_tries(reg);
        CHECK(!bg_too_many(t));
        reg = bg_tries_encode(t + 1);
        reg = bg_tries_encode(0);                          /* App 确认 */
    }

    /* 6. 复位原因：上电时 BOR + PIN 都是 1 → 上电；软件复位 SFT + PIN → 软件；看门狗 IWDG + PIN → 看门狗；只有 PIN → 按键 */
    CHECK(rr_decode((1u << 27) | (1u << 26)) == RR_POWER_ON);
    CHECK(rr_decode((1u << 28) | (1u << 26)) == RR_SOFTWARE);
    CHECK(rr_decode((1u << 29) | (1u << 26)) == RR_WATCHDOG);
    CHECK(rr_decode(1u << 26) == RR_PIN);
    CHECK(rr_decode(0) == RR_UNKNOWN);
    CHECK(rr_decode((1u << 29) | (1u << 28) | (1u << 27) | (1u << 26)) == RR_WATCHDOG);   /* 几次复位的标志叠在一起 */
    CHECK(strcmp(rr_name(RR_WATCHDOG), "watchdog") == 0 && strcmp(rr_name((rr_t)9), "?") == 0);

    printf("boot_guard: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
