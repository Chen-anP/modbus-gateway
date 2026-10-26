/*
 * test_wd.c - 任务看门狗：都报到才喂狗，一个任务卡住就不喂
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/wd.c tests/test_wd.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include "wd.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    wd_t w;
    uint32_t t, stuck = 99;
    int ok;

    /* 1. 3 个任务，超时 3000 ms。开机还没人报到：3 秒内照样喂狗 */
    wd_init(&w, 3, 3000, 1000);
    CHECK(wd_check(&w, 1000, &stuck) == 1);
    CHECK(wd_check(&w, 4000, &stuck) == 1);                /* 正好 3000：还不算超时 */
    CHECK(wd_check(&w, 4001, &stuck) == 0 && stuck == 0);  /* 一个都没报到：报第一个 */

    /* 2. 都正常报到：一直喂 */
    wd_init(&w, 3, 3000, 0);
    ok = 1;
    for (t = 0; t < 20000; t += 500) {
        wd_beat(&w, 0); wd_beat(&w, 1); wd_beat(&w, 2);
        ok &= wd_check(&w, t, &stuck);
    }
    CHECK(ok == 1);

    /* 3. ch2（编号 1）最后一次报到在 19500 ms，之后卡住：22500 还喂（正好 3 秒），22600 不喂，stuck = 1 */
    for (; t <= 22500; t += 500) {
        wd_beat(&w, 0); wd_beat(&w, 2);
        CHECK(wd_check(&w, t, &stuck) == 1);
    }
    wd_beat(&w, 0); wd_beat(&w, 2);
    CHECK(wd_check(&w, 22600, &stuck) == 0 && stuck == 1);

    /* 4. 它又活过来了：马上恢复喂狗 */
    wd_beat(&w, 0); wd_beat(&w, 1); wd_beat(&w, 2);
    CHECK(wd_check(&w, 24000, &stuck) == 1);

    /* 5. 报到很慢（每 2.5 秒一次）但没超过 3 秒：不算卡住 */
    wd_init(&w, 1, 3000, 0);
    ok = 1;
    for (t = 0; t < 30000; t += 500) {
        if (t % 2500 == 0) wd_beat(&w, 0);
        ok &= wd_check(&w, t, &stuck);
    }
    CHECK(ok == 1);

    /* 6. tick 计数回绕（0xFFFFFFFF 之后回到 0）也不会误判 */
    wd_init(&w, 1, 3000, 0xFFFFF000u);
    wd_beat(&w, 0);
    CHECK(wd_check(&w, 0xFFFFF800u, &stuck) == 1);
    CHECK(wd_check(&w, 0x00000300u, &stuck) == 1);          /* 回绕后：距上次报到 0x800 + 0x300 = 2816 ms，没超时 */
    CHECK(wd_check(&w, 0x00000400u, &stuck) == 0);          /* 0x800 + 0x400 = 3072 ms，超时 */
    /* 7. 事先声明要等 6 秒（转发固件、传感器擦 Flash）：这 6 秒里不报到也不算卡住，过了就算 */
    wd_init(&w, 2, 3000, 0);
    wd_beat(&w, 0); wd_beat(&w, 1);
    CHECK(wd_check(&w, 100, &stuck) == 1);
    wd_allow(&w, 1, 100, 6000);
    for (t = 500; t <= 6000; t += 500) {
        wd_beat(&w, 0);
        CHECK(wd_check(&w, t, &stuck) == 1);
    }
    wd_beat(&w, 0);
    CHECK(wd_check(&w, 6200, &stuck) == 0 && stuck == 1);
    wd_beat(&w, 1);
    CHECK(wd_check(&w, 6300, &stuck) == 1);

    printf("wd: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
