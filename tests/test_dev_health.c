/*
 * test_dev_health.c - 设备在线状态机
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/dev_health.c tests/test_dev_health.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include "dev_health.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    dev_health_t h;

    /* 1. 上电：UNKNOWN，该问 */
    dev_health_init(&h);
    CHECK(h.state == DEV_UNKNOWN && dev_health_due(&h, 0));

    /* 2. 第一次成功 → 变成 ONLINE，返回 +1；再成功返回 0 */
    CHECK(dev_health_report(&h, 1, 10) == +1 && h.state == DEV_ONLINE);
    CHECK(dev_health_report(&h, 1, 20) == 0);

    /* 3. 失败 1、2 次还算在线（偶发失败不跳状态），第 3 次掉线，返回 -1 */
    CHECK(dev_health_report(&h, 0, 100) == 0 && h.state == DEV_ONLINE);
    CHECK(dev_health_report(&h, 0, 200) == 0 && h.state == DEV_ONLINE);
    CHECK(dev_health_report(&h, 0, 300) == -1 && h.state == DEV_OFFLINE && h.offline_count == 1);

    /* 4. 掉线后 2 s 内不问，到点才问 */
    CHECK(!dev_health_due(&h, 300 + 1999));
    CHECK(dev_health_due(&h, 300 + 2000));

    /* 5. 重试又失败：不重复报掉线，下次重试时间往后推 2 s */
    CHECK(dev_health_report(&h, 0, 2300) == 0 && h.offline_count == 1);
    CHECK(!dev_health_due(&h, 4299) && dev_health_due(&h, 4300));

    /* 6. 重试成功 → 回到 ONLINE，返回 +1，连续失败清零 */
    CHECK(dev_health_report(&h, 1, 4300) == +1 && h.state == DEV_ONLINE && h.fails == 0);

    /* 7. 中间夹一次成功，连续失败重新计数：失败、失败、成功、失败、失败 → 不掉线 */
    dev_health_report(&h, 0, 5000);
    dev_health_report(&h, 0, 5100);
    dev_health_report(&h, 1, 5200);
    dev_health_report(&h, 0, 5300);
    CHECK(dev_health_report(&h, 0, 5400) == 0 && h.state == DEV_ONLINE);

    /* 8. 从来没成功过（设备没接）：UNKNOWN 失败 3 次直接 OFFLINE */
    dev_health_init(&h);
    dev_health_report(&h, 0, 0);
    dev_health_report(&h, 0, 100);
    CHECK(dev_health_report(&h, 0, 200) == -1 && h.state == DEV_OFFLINE);

    /* 9. 时间回绕：掉线时刻接近 0xFFFFFFFF，重试时刻越过 0 */
    dev_health_init(&h);
    for (int i = 0; i < 3; i++)
        dev_health_report(&h, 0, 0xFFFFFF00u);
    CHECK(h.state == DEV_OFFLINE);
    CHECK(!dev_health_due(&h, 0xFFFFFFFFu));               /* 才过 255 ms */
    CHECK(!dev_health_due(&h, 0x00000100u));               /* 回绕后 511 ms */
    CHECK(dev_health_due(&h, 0xFFFFFF00u + 2000u));        /* = 0x000006D0，到点 */

    /* 10. 统计 */
    CHECK(h.fail_count == 3 && h.ok_count == 0);
    CHECK(dev_state_name(DEV_ONLINE)[0] == 'o' && dev_state_name(DEV_UNKNOWN)[0] == 'u');

    printf("dev_health: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
