/*
 * boot_proto.c - Bootloader 的 Modbus 从机，说明见 boot_proto.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "boot_proto.h"

void boot_proto_init(boot_proto_t *b, uint8_t *buf, uint32_t cap, const af_layout_t *layout,
                     uint16_t boot_ver, uint16_t reason, uint16_t app_ver)
{
    memset(b, 0, sizeof(*b));
    file_rx_init(&b->rx, buf, cap);
    b->layout   = *layout;
    b->boot_ver = boot_ver;
    b->reason   = reason;
    b->app_ver  = app_ver;
}

static mb_ex_t bp_check(void *ctx, mb_table_t tab, uint16_t addr, uint16_t count, int is_write)
{
    (void)ctx;
    if (tab != MB_TAB_HOLDING)
        return MB_EX_ILLEGAL_ADDRESS;
    if (addr < BP_REG_CMD || (uint32_t)addr + count - 1u > BP_REG_LAST)
        return MB_EX_ILLEGAL_ADDRESS;
    if (is_write && (addr != BP_REG_CMD || count != 1u))       /* 只有 CMD 能写 */
        return MB_EX_ILLEGAL_ADDRESS;
    return MB_EX_NONE;
}

static mb_ex_t bp_read(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *value)
{
    const boot_proto_t *b = (const boot_proto_t *)ctx;
    (void)tab;

    switch (addr - BP_REG_CMD) {
    case 0: *value = 0;              break;
    case 1: *value = 1;              break;     /* STATE：在 Bootloader */
    case 2: *value = b->boot_ver;    break;
    case 3: *value = 0;              break;
    case 4: *value = b->last_err;    break;
    case 5: *value = b->reason;      break;
    case 6: *value = b->upg_state;   break;
    case 7: *value = b->app_ver;     break;
    default: return MB_EX_ILLEGAL_ADDRESS;
    }
    return MB_EX_NONE;
}

static mb_ex_t bp_write(void *ctx, mb_table_t tab, uint16_t addr, uint16_t value)
{
    boot_proto_t *b = (boot_proto_t *)ctx;
    (void)tab; (void)addr;

    switch (value) {
    case BP_CMD_REBOOT:     b->action = BP_ACT_REBOOT;  return MB_EX_NONE;
    case BP_CMD_RUN_APP:    b->action = BP_ACT_RUN_APP; return MB_EX_NONE;
    case BP_CMD_FORGET_APP: b->action = BP_ACT_FORGET_APP; return MB_EX_NONE;
    case BP_CMD_ENTER_BOOT:                             return MB_EX_NONE;     /* 已经在了 */
    default:                                            return MB_EX_ILLEGAL_VALUE;
    }
}

static mb_ex_t bp_file(void *ctx, uint16_t file_no, uint16_t record_no, const uint8_t *data, uint16_t len)
{
    boot_proto_t *b = (boot_proto_t *)ctx;
    mb_ex_t ex;
    af_err_t e;
    int complete = 0;

    if (file_no != FILE_NO_CENTRAL_FW)
        return MB_EX_ILLEGAL_ADDRESS;
    if (b->upg_state == BP_UPG_PROGRAMMING)         /* 正在写 Flash（板子上不会发生：写完才处理下一帧） */
        return MB_EX_BUSY;

    ex = file_rx_record(&b->rx, record_no, data, len, &complete);
    if (ex != MB_EX_NONE) {
        b->last_err  = (uint16_t)(BP_ERR_FILE | (unsigned)b->rx.last_err);
        b->upg_state = BP_UPG_FAILED;
        return ex;
    }
    if (record_no == 0) {                           /* 新文件开始 */
        b->upg_state = BP_UPG_RECEIVING;
        b->last_err  = 0;
    }
    if (!complete)
        return MB_EX_NONE;

    e = app_flash_check(&b->layout, b->rx.buf, b->rx.hdr.length, b->rx.hdr.load_addr);
    if (e != AF_OK) {                               /* 地址 / 长度 / 向量表不对：Flash 一个字节都没动 */
        b->last_err  = (uint16_t)(BP_ERR_UPGRADE | (unsigned)e);
        b->upg_state = BP_UPG_FAILED;
        return MB_EX_DEVICE_FAILURE;
    }
    b->upg_state = BP_UPG_PROGRAMMING;
    b->action    = BP_ACT_PROGRAM;
    return MB_EX_NONE;
}

void boot_proto_ops(boot_proto_t *b, mb_slave_ops_t *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->ctx         = b;
    ops->check       = bp_check;
    ops->read        = bp_read;
    ops->write       = bp_write;
    ops->file_record = bp_file;
}

uint16_t boot_proto_take_action(boot_proto_t *b)
{
    uint16_t a = b->action;
    b->action = BP_ACT_NONE;
    return a;
}

void boot_proto_program_done(boot_proto_t *b, af_err_t e)
{
    if (e == AF_OK) {
        b->upg_state = BP_UPG_DONE;
        b->app_ver   = (uint16_t)b->rx.hdr.version;
        b->last_err  = 0;
    } else {
        b->upg_state = BP_UPG_FAILED;
        b->last_err  = (uint16_t)(BP_ERR_UPGRADE | (unsigned)e);
    }
}
