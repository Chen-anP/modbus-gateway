/*
 * reg_space.c - 寄存器空间，说明见 reg_space.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "reg_space.h"

#define SYS_AREA_START  0xF000u

/* LAST_ERR 的编码：高字节是类别（0x01xx 点表错误见 point_table.h 的 pt_err_t；0x0200 设备拒绝写入） */
#define ERR_WRITE_REJECTED  0x0200u

static void lock(reg_space_t *rs)   { if (rs->lock)   rs->lock(); }
static void unlock(reg_space_t *rs) { if (rs->unlock) rs->unlock(); }

void reg_space_init(reg_space_t *rs, sys_regs_t *sys, void (*lk)(void), void (*ulk)(void))
{
    memset(rs, 0, sizeof(*rs));
    memset(rs->map, 0xFF, sizeof(rs->map));            /* 全部 -1 */
    rs->sys = sys;
    rs->lock = lk;
    rs->unlock = ulk;
}

void reg_space_load(reg_space_t *rs, const point_table_t *pt)
{
    lock(rs);
    rs->pt = pt;
    memset(rs->st, 0, sizeof(rs->st));
    memset(rs->map, 0xFF, sizeof(rs->map));
    for (uint16_t i = 0; i < pt->n; i++)
        rs->map[pt->pts[i].tab][pt->pts[i].gw_addr] = (int16_t)i;
    if (rs->sys)
        rs->sys->point_cnt = pt->n;
    unlock(rs);
}

/* ---------------------------------------------------------------- 从机回调 */

static int is_sys(mb_table_t tab, uint32_t addr)
{
    return tab == MB_TAB_HOLDING && addr >= SYS_AREA_START;
}

static mb_ex_t rs_check(void *ctx, mb_table_t tab, uint16_t addr, uint16_t count, int is_write)
{
    reg_space_t *rs = (reg_space_t *)ctx;
    uint32_t end = (uint32_t)addr + count;

    if (is_sys(tab, addr)) {                            /* 系统区：整段交给 sys_regs 检查 */
        mb_slave_ops_t sys_ops;
        if (rs->sys == NULL)
            return MB_EX_ILLEGAL_ADDRESS;
        sys_regs_ops(rs->sys, &sys_ops);
        return sys_ops.check(sys_ops.ctx, tab, addr, count, is_write);
    }
    if (end > PT_GW_ADDR_MAX + 1u)                      /* 映射区之外，或者跨进了系统区 */
        return MB_EX_ILLEGAL_ADDRESS;
    for (uint32_t a = addr; a < end; a++)
        if (rs->map[tab][a] < 0)                        /* 点表里没有这个地址 */
            return MB_EX_ILLEGAL_ADDRESS;
    return MB_EX_NONE;
}

static mb_ex_t rs_read(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *value)
{
    reg_space_t *rs = (reg_space_t *)ctx;
    const rs_point_t *p;

    if (is_sys(tab, addr)) {
        mb_slave_ops_t sys_ops;
        sys_regs_ops(rs->sys, &sys_ops);
        return sys_ops.read(sys_ops.ctx, tab, addr, value);
    }
    p = &rs->st[rs->map[tab][addr]];                    /* rs_check 已经保证映射存在 */
    *value = (p->flags & RS_PENDING) ? p->pend_value : p->value;
    return MB_EX_NONE;
}

static mb_ex_t rs_write(void *ctx, mb_table_t tab, uint16_t addr, uint16_t value)
{
    reg_space_t *rs = (reg_space_t *)ctx;
    rs_point_t *p;

    if (is_sys(tab, addr)) {
        mb_slave_ops_t sys_ops;
        sys_regs_ops(rs->sys, &sys_ops);
        return sys_ops.write(sys_ops.ctx, tab, addr, value);
    }
    p = &rs->st[rs->map[tab][addr]];
    p->pend_value = value;
    p->flags |= RS_PENDING;                             /* 只做标记，下发由通道任务做 */
    rs->writes++;
    return MB_EX_NONE;
}

void reg_space_ops(reg_space_t *rs, mb_slave_ops_t *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->ctx = rs;
    ops->check = rs_check;
    ops->read = rs_read;
    ops->write = rs_write;
    ops->file_record = NULL;                            /* 0x15 第 6 步 */
}

/* ---------------------------------------------------------------- 通道任务用 */

int reg_space_next_pending(reg_space_t *rs, uint8_t ch, uint16_t from, uint16_t *value)
{
    int found = -1;

    lock(rs);
    if (rs->pt) {
        for (uint16_t i = from; i < rs->pt->n; i++) {
            if (rs->pt->pts[i].ch == ch && (rs->st[i].flags & RS_PENDING)) {
                *value = rs->st[i].pend_value;
                found = i;
                break;
            }
        }
    }
    unlock(rs);
    return found;
}

void reg_space_write_result(reg_space_t *rs, uint16_t idx, uint16_t written, rs_write_result_t r)
{
    rs_point_t *p = &rs->st[idx];

    lock(rs);
    if (r == RS_WRITE_OK) {
        p->value = written;
        p->flags |= RS_SYNCED;
        if (p->pend_value == written)       /* 期间上位机没有再写别的值 */
            p->flags &= (uint8_t)~RS_PENDING;
    } else if (r == RS_WRITE_REJECTED) {
        if (p->pend_value == written)
            p->flags &= (uint8_t)~RS_PENDING;   /* 设备不接受：放弃，下一次读会把真实值读回来 */
        if (rs->sys)
            rs->sys->last_err = ERR_WRITE_REJECTED;
    }
    unlock(rs);
}

int reg_space_update(reg_space_t *rs, uint16_t idx, uint16_t value)
{
    rs_point_t *p = &rs->st[idx];
    int changed = 0;

    lock(rs);
    if (!(p->flags & RS_PENDING)) {
        changed = !(p->flags & RS_SYNCED) || p->value != value;
        p->value = value;
        p->flags |= RS_SYNCED;
    }
    unlock(rs);
    return changed;
}

uint16_t reg_space_get(reg_space_t *rs, uint16_t idx)
{
    uint16_t v;

    lock(rs);
    v = (rs->st[idx].flags & RS_PENDING) ? rs->st[idx].pend_value : rs->st[idx].value;
    unlock(rs);
    return v;
}
