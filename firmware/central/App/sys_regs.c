/*
 * sys_regs.c - 系统区寄存器，说明见 sys_regs.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "sys_regs.h"

#define CH_STAT_END (SYS_REG_CH_STAT + 4u * SYS_CH_COUNT)     /* 0xF028，不含 */

void sys_regs_init(sys_regs_t *s, uint16_t fw_ver, void (*get_ch_stat)(uint8_t ch, sys_ch_stat_t *out))
{
    memset(s, 0, sizeof(*s));
    s->fw_ver      = fw_ver;
    s->get_ch_stat = get_ch_stat;
}

/* 这个地址有没有定义 */
static int defined(uint16_t a)
{
    return (a >= SYS_REG_CMD && a <= SYS_REG_BOOT_TRIES) || a == SYS_REG_TARGET || a == SYS_REG_TARGET_CMD
        || (a >= SYS_REG_CH_STAT && a < CH_STAT_END);
}

/* 先检查整段地址：mb_slave_handle 会先调用它，全部合法才逐个读写（多寄存器写入要么全写，要么一个不写） */
static mb_ex_t sys_check(void *ctx, mb_table_t tab, uint16_t addr, uint16_t count, int is_write)
{
    (void)ctx;
    if (tab != MB_TAB_HOLDING)
        return MB_EX_ILLEGAL_ADDRESS;
    for (uint32_t a = addr; a < (uint32_t)addr + count; a++) {
        if (!defined((uint16_t)a))
            return MB_EX_ILLEGAL_ADDRESS;
        if (is_write && a != SYS_REG_CMD && a != SYS_REG_TARGET)   /* 只有 CMD、TARGET 能写，其余只读 */
            return MB_EX_ILLEGAL_ADDRESS;
    }
    return MB_EX_NONE;
}

static mb_ex_t sys_read(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *value)
{
    sys_regs_t *s = (sys_regs_t *)ctx;
    (void)tab;

    switch (addr) {
    case SYS_REG_CMD:       *value = 0;            return MB_EX_NONE;
    case SYS_REG_STATE:     *value = s->state;     return MB_EX_NONE;
    case SYS_REG_FW_VER:    *value = s->fw_ver;    return MB_EX_NONE;
    case SYS_REG_POINT_CNT: *value = s->point_cnt; return MB_EX_NONE;
    case SYS_REG_LAST_ERR:  *value = s->last_err;  return MB_EX_NONE;
    case SYS_REG_PT_SOURCE: *value = s->pt_source; return MB_EX_NONE;
    case SYS_REG_RESET_RSN: *value = s->reset_reason; return MB_EX_NONE;
    case SYS_REG_BOOT_TRIES: *value = s->boot_tries; return MB_EX_NONE;
    case SYS_REG_TARGET:    *value = s->target;    return MB_EX_NONE;
    case SYS_REG_TARGET_CMD: *value = 0;           return MB_EX_NONE;
    default: break;
    }
    if (addr >= SYS_REG_CH_STAT && addr < CH_STAT_END) {
        sys_ch_stat_t st;
        uint16_t off = (uint16_t)(addr - SYS_REG_CH_STAT);

        memset(&st, 0, sizeof(st));
        if (s->get_ch_stat)
            s->get_ch_stat((uint8_t)(off / 4u + 1u), &st);
        switch (off % 4u) {
        case 0:  *value = (uint16_t)st.ok;         break;
        case 1:  *value = (uint16_t)st.timeouts;   break;
        case 2:  *value = (uint16_t)st.crc_errors; break;
        default: *value = (uint16_t)st.offline;    break;
        }
        return MB_EX_NONE;
    }
    return MB_EX_ILLEGAL_ADDRESS;
}

static mb_ex_t sys_write(void *ctx, mb_table_t tab, uint16_t addr, uint16_t value)
{
    sys_regs_t *s = (sys_regs_t *)ctx;
    (void)tab;

    if (addr == SYS_REG_TARGET) {                   /* 通道 1～2、地址 1～247 */
        uint8_t ch = (uint8_t)(value >> 8), dev = (uint8_t)value;
        if (ch < 1u || ch > SYS_CH_COUNT || dev < 1u || dev > 247u)
            return MB_EX_ILLEGAL_VALUE;
        s->target = value;
        return MB_EX_NONE;
    }
    if (addr != SYS_REG_CMD)
        return MB_EX_ILLEGAL_ADDRESS;
    switch (value) {
    case SYS_CMD_REBOOT:
    case SYS_CMD_CLEAR_TABLE:
    case SYS_CMD_ENTER_BOOT:
    case SYS_CMD_CLEAR_STAY:
    case SYS_CMD_TEST_HANG:
    case SYS_CMD_TEST_CRASH:
        break;
    default:                                        /* 第 8 步的转发命令等以后再加 */
        return MB_EX_ILLEGAL_VALUE;
    }
    s->pending_cmd = value;                         /* 只记下来：现在重启，回应就发不出去了 */
    return MB_EX_NONE;
}

void sys_regs_ops(sys_regs_t *s, mb_slave_ops_t *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->ctx         = s;
    ops->check       = sys_check;
    ops->read        = sys_read;
    ops->write       = sys_write;
    ops->file_record = NULL;                        /* 0x15 第 6 步再接 */
}

uint16_t sys_regs_take_cmd(sys_regs_t *s)
{
    uint16_t c = s->pending_cmd;
    s->pending_cmd = SYS_CMD_NONE;
    return c;
}
