/*
 * dev_health.c - 设备在线状态，说明见 dev_health.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "dev_health.h"

void dev_health_init(dev_health_t *h)
{
    memset(h, 0, sizeof(*h));
    h->state = DEV_UNKNOWN;
}

/* 时间比较要考虑回绕：uint32_t 毫秒计数约 49.7 天回到 0。
 * 用"差值转成有符号数"判断先后，只要两个时刻相差不超过 24.8 天就是对的 */
static int time_reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

int dev_health_due(const dev_health_t *h, uint32_t now_ms)
{
    if (h->state != DEV_OFFLINE)
        return 1;
    return time_reached(now_ms, h->retry_at_ms);
}

int dev_health_report(dev_health_t *h, int alive, uint32_t now_ms)
{
    if (alive) {
        h->ok_count++;
        h->fails = 0;
        if (h->state != DEV_ONLINE) {
            h->state = DEV_ONLINE;
            return +1;
        }
        return 0;
    }

    h->fail_count++;
    if (h->fails < 0xFFu)
        h->fails++;

    if (h->state == DEV_OFFLINE) {          /* 已经掉线：这次重试也失败了，再等一个周期 */
        h->retry_at_ms = now_ms + DEV_OFFLINE_RETRY_MS;
        return 0;
    }
    if (h->fails >= DEV_OFFLINE_AFTER) {
        h->state = DEV_OFFLINE;
        h->offline_count++;
        h->retry_at_ms = now_ms + DEV_OFFLINE_RETRY_MS;
        return -1;
    }
    return 0;
}

const char *dev_state_name(dev_state_t s)
{
    switch (s) {
    case DEV_ONLINE:  return "online";
    case DEV_OFFLINE: return "offline";
    default:          return "unknown";
    }
}
