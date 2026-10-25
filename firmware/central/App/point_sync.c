/*
 * point_sync.c - 点同步，说明见 point_sync.h
 *
 * 日志：
 *   INFO   设备上线、开关量（0x / 1x）的值变化、汇总
 *   WARN   设备掉线、设备拒绝写入、读块回异常码（同一个块只在结果变化时报）
 *   DEBUG  寄存器（3x / 4x）的值变化、每一帧收发（mb_master）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "log.h"
#include "point_sync.h"

static const char k_tab[MB_TAB_COUNT][3] = { "0x", "1x", "3x", "4x" };

static ps_dev_t *find_dev(point_sync_t *ps, uint8_t addr)
{
    for (uint8_t i = 0; i < ps->ndev; i++)
        if (ps->devs[i].addr == addr)
            return &ps->devs[i];
    return NULL;
}

int point_sync_init(point_sync_t *ps, uint8_t ch, mb_master_t *mb, reg_space_t *rs, const point_table_t *pt)
{
    memset(ps, 0, sizeof(*ps));
    ps->ch = ch;
    ps->mb = mb;
    ps->rs = rs;
    ps->pt = pt;
    for (uint16_t i = 0; i < pt->n; i++) {
        const pt_point_t *p = &pt->pts[i];
        if (p->ch != ch || find_dev(ps, p->dev))
            continue;
        if (ps->ndev >= PS_MAX_DEVS)
            return -1;
        ps->devs[ps->ndev].addr = p->dev;
        dev_health_init(&ps->devs[ps->ndev].health);
        ps->ndev++;
    }
    return 0;
}

static uint32_t now_ms(const point_sync_t *ps)
{
    return ps->mb->io->now_ms();
}

/* 能不能问这个设备：掉线中且没到重试时间就不问 */
static int due(const point_sync_t *ps, const ps_dev_t *d)
{
    return dev_health_due(&d->health, now_ms(ps));
}

/* 报告一次结果，状态变了打日志 */
static void report(point_sync_t *ps, ps_dev_t *d, int rc)
{
    int alive = (rc == MB_OK || rc == MB_ERR_EXCEPTION);   /* 回了异常码也说明设备在 */
    int change = dev_health_report(&d->health, alive, now_ms(ps));
    const char *name = ps->mb->io->name;

    if (change > 0)
        LOGI(name, "dev %u online", (unsigned)d->addr);
    else if (change < 0)
        LOGW(name, "dev %u offline after %u failures, retry every %u ms",
             (unsigned)d->addr, (unsigned)DEV_OFFLINE_AFTER, (unsigned)DEV_OFFLINE_RETRY_MS);
}

/* 掉线的设备只试 1 次，不重试（第 3 步的做法） */
static int with_retries(point_sync_t *ps, const ps_dev_t *d)
{
    uint8_t saved = ps->mb->retries;
    if (d->health.state == DEV_OFFLINE)
        ps->mb->retries = 0;
    return saved;
}

static void do_writes(point_sync_t *ps)
{
    const char *name = ps->mb->io->name;
    uint16_t value;
    int idx = 0;

    while ((idx = reg_space_next_pending(ps->rs, ps->ch, (uint16_t)idx, &value)) >= 0) {
        const pt_point_t *p = &ps->pt->pts[idx];
        ps_dev_t *d = find_dev(ps, p->dev);
        mb_ex_t ex = MB_EX_NONE;
        uint8_t saved;
        int rc;

        if (d == NULL || !due(ps, d)) {         /* 设备掉线中：保留标记，等它回来 */
            idx++;
            continue;
        }
        saved = (uint8_t)with_retries(ps, d);
        rc = mb_master_write_single(ps->mb, p->dev, (mb_table_t)p->tab, p->dev_addr, value, &ex);
        ps->mb->retries = saved;
        report(ps, d, rc);

        if (rc == MB_OK) {
            reg_space_write_result(ps->rs, (uint16_t)idx, value, RS_WRITE_OK);
            ps->writes_ok++;
            LOGI(name, "write %s[%u] = %u -> dev %u %s[%u] ok", k_tab[p->tab], (unsigned)p->gw_addr,
                 (unsigned)value, (unsigned)p->dev, k_tab[p->tab], (unsigned)p->dev_addr);
        } else if (rc == MB_ERR_EXCEPTION) {
            reg_space_write_result(ps->rs, (uint16_t)idx, value, RS_WRITE_REJECTED);
            ps->writes_rejected++;
            LOGW(name, "write %s[%u] rejected by dev %u: exception %u", k_tab[p->tab], (unsigned)p->gw_addr,
                 (unsigned)p->dev, (unsigned)ex);
        } else {
            reg_space_write_result(ps->rs, (uint16_t)idx, value, RS_WRITE_RETRY);
            ps->writes_retry++;
        }
        idx++;
    }
}

static void do_reads(point_sync_t *ps)
{
    const char *name = ps->mb->io->name;
    uint16_t v[PT_BLOCK_MAX];

    for (uint16_t b = 0; b < ps->pt->nblocks; b++) {
        const pt_block_t *blk = &ps->pt->blocks[b];
        ps_dev_t *d;
        mb_ex_t ex = MB_EX_NONE;
        uint8_t saved;
        int rc;

        if (blk->ch != ps->ch)
            continue;
        d = find_dev(ps, blk->dev);
        if (d == NULL || !due(ps, d))
            continue;
        saved = (uint8_t)with_retries(ps, d);
        rc = mb_master_read(ps->mb, blk->dev, (mb_table_t)blk->tab, blk->start, blk->count, v, PT_BLOCK_MAX, &ex);
        ps->mb->retries = saved;
        report(ps, d, rc);

        if (rc == MB_OK) {
            ps->reads_ok++;
            for (uint16_t i = 0; i < blk->count; i++) {
                uint16_t idx = (uint16_t)(blk->first + i);
                const pt_point_t *p = &ps->pt->pts[idx];
                if (!reg_space_update(ps->rs, idx, v[i]))
                    continue;
                if (p->tab == MB_TAB_COIL || p->tab == MB_TAB_DISCRETE)
                    LOGI(name, "%s[%u] = %u (dev %u)", k_tab[p->tab], (unsigned)p->gw_addr, (unsigned)v[i], (unsigned)p->dev);
                else
                    LOGD(name, "%s[%u] = %u (dev %u)", k_tab[p->tab], (unsigned)p->gw_addr, (unsigned)v[i], (unsigned)p->dev);
            }
        } else if (rc == MB_ERR_EXCEPTION && ps->blk_rc[b] != rc) {
            LOGW(name, "dev %u %s[%u..%u]: exception %u (check point table)", (unsigned)blk->dev, k_tab[blk->tab],
                 (unsigned)blk->start, (unsigned)(blk->start + blk->count - 1), (unsigned)ex);
        }
        ps->blk_rc[b] = (int8_t)rc;
    }
}

void point_sync_step(point_sync_t *ps)
{
    ps->rounds++;
    do_writes(ps);
    do_reads(ps);
}

uint8_t point_sync_offline(const point_sync_t *ps)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ps->ndev; i++)
        if (ps->devs[i].health.state == DEV_OFFLINE)
            n++;
    return n;
}

void point_sync_report(const point_sync_t *ps)
{
    const mb_master_stats_t *s = &ps->mb->st;
    const char *name = ps->mb->io->name;

    LOGI(name, "rounds %lu reads %lu | writes ok %lu retry %lu rejected %lu | timeout %lu crc %lu frame %lu",
         (unsigned long)ps->rounds, (unsigned long)ps->reads_ok, (unsigned long)ps->writes_ok,
         (unsigned long)ps->writes_retry, (unsigned long)ps->writes_rejected,
         (unsigned long)s->timeouts, (unsigned long)s->crc_errors, (unsigned long)s->frame_errors);
    for (uint8_t i = 0; i < ps->ndev; i++)
        LOGI(name, "dev %u %-7s ok %lu fail %lu", (unsigned)ps->devs[i].addr, dev_state_name(ps->devs[i].health.state),
             (unsigned long)ps->devs[i].health.ok_count, (unsigned long)ps->devs[i].health.fail_count);
}
