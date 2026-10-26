/*
 * wd.c - 任务看门狗的判断逻辑，说明见 wd.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "wd.h"

void wd_init(wd_t *w, uint32_t n, uint32_t timeout_ms, uint32_t now_ms)
{
    uint32_t i;

    memset((void *)w, 0, sizeof(*w));
    w->n = n > WD_MAX_TASKS ? WD_MAX_TASKS : n;
    w->timeout_ms = timeout_ms;
    for (i = 0; i < w->n; i++)
        w->last_ok[i] = now_ms;             /* 开机时给每个任务一个完整的 timeout */
}

void wd_beat(wd_t *w, uint32_t id)
{
    if (id < w->n)
        w->beat[id]++;
}

void wd_allow(wd_t *w, uint32_t id, uint32_t now_ms, uint32_t ms)
{
    if (id < w->n)
        w->allow_until[id] = now_ms + ms;
}

int wd_check(wd_t *w, uint32_t now_ms, uint32_t *stuck)
{
    uint32_t i;
    int ok = 1;

    for (i = 0; i < w->n; i++) {
        uint32_t b = w->beat[i];
        if (b != w->seen[i]) {
            w->seen[i] = b;
            w->last_ok[i] = now_ms;
        } else if (now_ms - w->last_ok[i] > w->timeout_ms           /* 无符号相减：tick 回绕也对 */
                   && (int32_t)(now_ms - w->allow_until[i]) >= 0) {
            if (ok && stuck)
                *stuck = i;
            ok = 0;
        }
    }
    return ok;
}
