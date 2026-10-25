/*
 * poll.c - 主站轮询，说明见 poll.h
 *
 * 日志等级的安排（INFO 下不刷屏，DEBUG 下什么都看得到）：
 *   INFO   设备上线 / 掉线、第一次读到的值、开关量变化、每 10 秒一次的汇总
 *   WARN   掉线、异常码（同一个异常码只报一次）
 *   DEBUG  每次读到的值、每一帧的收发字节（mb_master 打印）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "poll.h"
#include "log.h"

static const char k_tab_prefix[MB_TAB_COUNT][3] = { "0x", "1x", "3x", "4x" };

static int is_bits(mb_table_t t)
{
    return t == MB_TAB_COIL || t == MB_TAB_DISCRETE;
}

/* 把值格式化成 "3x[0..1] = 266, 455" 或 "1x[0..2] = 1 0 1" */
static void format_values(char *buf, size_t size, const poll_dev_cfg_t *c, const uint16_t *v)
{
    int n = snprintf(buf, size, "%s[%u..%u] =", k_tab_prefix[c->tab],
                     (unsigned)c->start, (unsigned)(c->start + c->count - 1));

    for (uint16_t i = 0; i < c->count && n > 0 && (size_t)n < size; i++)
        n += snprintf(&buf[n], size - (size_t)n, is_bits(c->tab) ? " %u" : (i ? ", %u" : " %u"), (unsigned)v[i]);
}

int poll_ch_init(poll_ch_t *ch, mb_master_t *mb, poll_dev_t *devs, const poll_dev_cfg_t *cfgs, uint8_t ndev)
{
    uint32_t now = mb->io->now_ms();

    ch->mb   = mb;
    ch->devs = devs;
    ch->ndev = ndev;
    for (uint8_t i = 0; i < ndev; i++) {
        if (cfgs[i].count == 0 || cfgs[i].count > POLL_MAX_VALUES || cfgs[i].tab >= MB_TAB_COUNT)
            return -1;
        memset(&devs[i], 0, sizeof(devs[i]));
        devs[i].cfg     = &cfgs[i];
        devs[i].next_ms = now;              /* 上电后马上读一次 */
        dev_health_init(&devs[i].health);
    }
    return 0;
}

static int time_reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

/* 读一个设备并处理结果 */
static void poll_one(poll_ch_t *ch, poll_dev_t *d)
{
    const poll_dev_cfg_t *c = d->cfg;
    const char *name = ch->mb->io->name;
    uint16_t v[POLL_MAX_VALUES];
    char text[80];
    mb_ex_t ex = MB_EX_NONE;
    int rc, change, alive;

    /* 已经掉线的设备只试 1 次、不重试：它大概率还是不在，每次少等 200 ms，同一通道上的其他设备就少等 200 ms */
    if (d->health.state == DEV_OFFLINE) {
        uint8_t saved = ch->mb->retries;
        ch->mb->retries = 0;
        rc = mb_master_read(ch->mb, c->addr, c->tab, c->start, c->count, v, POLL_MAX_VALUES, &ex);
        ch->mb->retries = saved;
    } else {
        rc = mb_master_read(ch->mb, c->addr, c->tab, c->start, c->count, v, POLL_MAX_VALUES, &ex);
    }
    alive = (rc == MB_OK || rc == MB_ERR_EXCEPTION);       /* 回了异常码也说明设备在 */
    change = dev_health_report(&d->health, alive, ch->mb->io->now_ms());

    if (change > 0)
        LOGI(name, "dev %u %s: online", (unsigned)c->addr, c->name);
    else if (change < 0)
        LOGW(name, "dev %u %s: offline after %u failures, retry every %u ms",
             (unsigned)c->addr, c->name, (unsigned)DEV_OFFLINE_AFTER, (unsigned)DEV_OFFLINE_RETRY_MS);

    if (rc == MB_OK) {
        int first   = !d->have_values;
        int changed = memcmp(d->values, v, c->count * sizeof(v[0])) != 0;

        memcpy(d->values, v, c->count * sizeof(v[0]));
        d->have_values = 1;
        format_values(text, sizeof(text), c, v);
        if (first || (changed && is_bits(c->tab)))
            LOGI(name, "dev %u %s: %s", (unsigned)c->addr, c->name, text);
        else
            LOGD(name, "dev %u %s: %s", (unsigned)c->addr, c->name, text);
    } else if (rc == MB_ERR_EXCEPTION && (d->last_rc != rc || d->last_ex != ex)) {
        LOGW(name, "dev %u %s: exception %u", (unsigned)c->addr, c->name, (unsigned)ex);
    }
    d->last_rc = rc;
    d->last_ex = ex;
}

uint8_t poll_ch_step(poll_ch_t *ch)
{
    uint8_t polled = 0;

    for (uint8_t i = 0; i < ch->ndev; i++) {
        poll_dev_t *d = &ch->devs[i];
        uint32_t now = ch->mb->io->now_ms();        /* 每个设备都重新取：前面的设备可能花了时间 */

        if (!time_reached(now, d->next_ms) || !dev_health_due(&d->health, now))
            continue;
        d->next_ms = now + d->cfg->period_ms;
        poll_one(ch, d);
        polled++;
    }
    return polled;
}

void poll_ch_report(const poll_ch_t *ch)
{
    const mb_master_stats_t *s = &ch->mb->st;
    const char *name = ch->mb->io->name;
    char text[80];

    LOGI(name, "req %lu ok %lu ex %lu | timeout %lu crc %lu frame %lu send %lu retry %lu",
         (unsigned long)s->requests, (unsigned long)s->ok, (unsigned long)s->exceptions,
         (unsigned long)s->timeouts, (unsigned long)s->crc_errors, (unsigned long)s->frame_errors,
         (unsigned long)s->send_errors, (unsigned long)s->retries);

    for (uint8_t i = 0; i < ch->ndev; i++) {
        const poll_dev_t *d = &ch->devs[i];

        if (d->have_values)
            format_values(text, sizeof(text), d->cfg, d->values);
        else
            snprintf(text, sizeof(text), "no data");
        LOGI(name, "dev %u %-6s %-7s ok %lu fail %lu | %s",
             (unsigned)d->cfg->addr, d->cfg->name, dev_state_name(d->health.state),
             (unsigned long)d->health.ok_count, (unsigned long)d->health.fail_count, text);
    }
}

const poll_dev_t *poll_ch_find(const poll_ch_t *ch, uint8_t addr)
{
    for (uint8_t i = 0; i < ch->ndev; i++)
        if (ch->devs[i].cfg->addr == addr)
            return &ch->devs[i];
    return NULL;
}
