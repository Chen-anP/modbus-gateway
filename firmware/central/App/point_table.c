/*
 * point_table.c - 点表解析、校验、排序、合并读块，说明见 point_table.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "point_table.h"

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* 排序键：通道、设备、表、传感器地址。同一设备同一张表的点挨在一起，地址从小到大 */
static int before(const pt_point_t *a, const pt_point_t *b)
{
    if (a->ch != b->ch)   return a->ch < b->ch;
    if (a->dev != b->dev) return a->dev < b->dev;
    if (a->tab != b->tab) return a->tab < b->tab;
    return a->dev_addr < b->dev_addr;
}

static void build_blocks(point_table_t *t)
{
    t->nblocks = 0;
    for (uint16_t i = 0; i < t->n; i++) {
        const pt_point_t *p = &t->pts[i];
        pt_block_t *b = t->nblocks ? &t->blocks[t->nblocks - 1] : NULL;

        /* 能接在上一个块后面：同通道、同设备、同表、地址正好是下一个、块还没满 */
        if (b && b->ch == p->ch && b->dev == p->dev && b->tab == p->tab
            && p->dev_addr == (uint16_t)(b->start + b->count) && b->count < PT_BLOCK_MAX) {
            b->count++;
            continue;
        }
        b = &t->blocks[t->nblocks++];
        b->ch = p->ch;
        b->dev = p->dev;
        b->tab = p->tab;
        b->start = p->dev_addr;
        b->count = 1;
        b->first = i;
    }
}

pt_err_t point_table_parse(point_table_t *t, const uint8_t *buf, uint32_t len, uint16_t *bad)
{
    uint16_t n, dummy;

    if (bad == NULL)
        bad = &dummy;
    *bad = 0;
    if (len == 0 || len % PT_REC_SIZE != 0)
        return PT_ERR_LEN;
    if (len / PT_REC_SIZE > PT_MAX_POINTS)
        return PT_ERR_COUNT;
    n = (uint16_t)(len / PT_REC_SIZE);

    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *r = &buf[i * PT_REC_SIZE];
        pt_point_t p;

        *bad = i;
        p.tab = r[0];
        p.ch = r[1];
        p.dev = r[2];
        p.gw_addr = get16(&r[4]);
        p.dev_addr = get16(&r[6]);
        if (p.tab >= MB_TAB_COUNT)              return PT_ERR_TAB;
        if (p.ch < 1 || p.ch > PT_CH_COUNT)     return PT_ERR_CH;
        if (p.dev < 1 || p.dev > 247)           return PT_ERR_DEV;
        if (p.gw_addr > PT_GW_ADDR_MAX)         return PT_ERR_ADDR;
        for (uint16_t j = 0; j < i; j++)        /* 前面已经放进去的点里有没有同一个中控地址 */
            if (t->pts[j].tab == p.tab && t->pts[j].gw_addr == p.gw_addr)
                return PT_ERR_DUP;
        t->pts[i] = p;
    }

    /* 插入排序：点数不多（≤ 128），只在加载时做一次 */
    for (uint16_t i = 1; i < n; i++) {
        pt_point_t p = t->pts[i];
        uint16_t j = i;
        while (j > 0 && before(&p, &t->pts[j - 1])) {
            t->pts[j] = t->pts[j - 1];
            j--;
        }
        t->pts[j] = p;
    }
    t->n = n;
    build_blocks(t);
    return PT_OK;
}

uint32_t point_table_encode(const pt_point_t *pts, uint16_t n, uint8_t *buf, uint32_t size)
{
    if ((uint32_t)n * PT_REC_SIZE > size)
        return 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t *r = &buf[i * PT_REC_SIZE];
        r[0] = pts[i].tab;
        r[1] = pts[i].ch;
        r[2] = pts[i].dev;
        r[3] = 0;
        put16(&r[4], pts[i].gw_addr);
        put16(&r[6], pts[i].dev_addr);
    }
    return (uint32_t)n * PT_REC_SIZE;
}

int point_table_find(const point_table_t *t, mb_table_t tab, uint16_t gw_addr)
{
    for (uint16_t i = 0; i < t->n; i++)
        if (t->pts[i].tab == tab && t->pts[i].gw_addr == gw_addr)
            return i;
    return -1;
}

const char *pt_err_name(pt_err_t e)
{
    static const char *const k[] = { "ok", "bad length", "too many points", "bad table",
                                     "bad channel", "bad device address", "gw address > 499", "duplicate" };
    return ((unsigned)e < sizeof(k) / sizeof(k[0])) ? k[e] : "?";
}
