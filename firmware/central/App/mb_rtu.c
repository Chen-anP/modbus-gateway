/*
 * mb_rtu.c - 轻量 Modbus RTU 协议层，说明见 mb_rtu.h
 *
 * 参考规范：Modbus Application Protocol V1.1b3、Modbus over Serial Line V1.02
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "mb_rtu.h"

/* 规范规定的数量上限 */
#define MAX_READ_BITS     2000u
#define MAX_READ_REGS     125u
#define MAX_WRITE_BITS    1968u
#define MAX_WRITE_REGS    123u
#define FILE_BC_MIN       0x07u
#define FILE_BC_MAX       0xF5u     /* 子请求数据最多 0xF5 - 7 = 238 字节 */
#define FILE_REF_TYPE     0x06u

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* 末尾追加 CRC（低字节在前），返回总长度 */
static uint16_t add_crc(uint8_t *adu, uint16_t len)
{
    uint16_t crc = mb_crc16(adu, len);
    adu[len]     = (uint8_t)crc;
    adu[len + 1] = (uint8_t)(crc >> 8);
    return (uint16_t)(len + 2);
}

static int crc_ok(const uint8_t *adu, uint16_t len)
{
    return len >= 4 && mb_crc16(adu, (uint16_t)(len - 2)) == (uint16_t)(adu[len - 2] | (adu[len - 1] << 8));
}

uint16_t mb_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* ================================================================ 帧长度 */

int mb_frame_need(const uint8_t *buf, uint16_t have, int is_request)
{
    int total;
    uint8_t fc;

    if (have < 2)
        return 0;
    fc = buf[1];

    if (is_request) {
        switch (fc) {
        case MB_FC_READ_COILS: case MB_FC_READ_DISCRETE:
        case MB_FC_READ_HOLDING: case MB_FC_READ_INPUT:
        case MB_FC_WRITE_COIL: case MB_FC_WRITE_REG:
            total = 8;                              /* 地址 功能码 起始(2) 数量/数值(2) CRC(2) */
            break;
        case MB_FC_WRITE_COILS: case MB_FC_WRITE_REGS:
            if (have < 7) return 0;
            total = 9 + buf[6];                     /* ... 数量(2) 字节数(1) 数据 CRC */
            break;
        case MB_FC_WRITE_FILE_RECORD:
            if (have < 3) return 0;
            total = 5 + buf[2];                     /* 地址 功能码 字节数 数据 CRC */
            break;
        default:
            return -1;
        }
    } else {
        if (fc & 0x80)
            return 5;                               /* 异常回应：地址 功能码|0x80 异常码 CRC */
        switch (fc) {
        case MB_FC_READ_COILS: case MB_FC_READ_DISCRETE:
        case MB_FC_READ_HOLDING: case MB_FC_READ_INPUT:
        case MB_FC_WRITE_FILE_RECORD:               /* 0x15 回应是请求的回显，格式相同 */
            if (have < 3) return 0;
            total = 5 + buf[2];
            break;
        case MB_FC_WRITE_COIL: case MB_FC_WRITE_REG:
        case MB_FC_WRITE_COILS: case MB_FC_WRITE_REGS:
            total = 8;
            break;
        default:
            return -1;
        }
    }
    return (total <= (int)MB_ADU_MAX) ? total : -1;
}

/* ================================================================ 帧组装器 */

void mb_framer_init(mb_framer_t *f, int is_request, uint32_t gap_ms)
{
    memset(f, 0, sizeof(*f));
    f->is_request = (uint8_t)(is_request != 0);
    f->gap_ms = gap_ms;
}

void mb_framer_reset(mb_framer_t *f)
{
    f->len = 0;
}

int mb_framer_feed(mb_framer_t *f, const uint8_t *data, uint16_t len, uint32_t now_ms, uint16_t *used)
{
    uint16_t i;
    int need;

    /* 距离上一个字节太久：之前的半帧作废（对方中途停发、或者我们从一帧中间开始收） */
    if (f->len > 0 && (uint32_t)(now_ms - f->last_ms) > f->gap_ms) {
        f->dropped++;
        f->len = 0;
    }
    if (len > 0)
        f->last_ms = now_ms;

    for (i = 0; i < len; i++) {
        f->buf[f->len++] = data[i];
        need = mb_frame_need(f->buf, f->len, f->is_request);
        if (need < 0) {                 /* 不认识的功能码：整段丢掉，等下一个帧间隔重新同步 */
            f->dropped++;
            f->len = 0;
            continue;
        }
        if (need > 0 && f->len >= (uint16_t)need) {
            if (used) *used = (uint16_t)(i + 1);
            return 1;
        }
        if (f->len >= MB_ADU_MAX) {     /* 理论上不会到这里，防御 */
            f->dropped++;
            f->len = 0;
        }
    }
    if (used) *used = len;
    return 0;
}

/* ================================================================ 从机 */

static mb_ex_t check_range(const mb_slave_ops_t *ops, mb_table_t tab, uint16_t addr, uint16_t n, int wr)
{
    if ((uint32_t)addr + n > 0x10000u)
        return MB_EX_ILLEGAL_ADDRESS;
    return ops->check(ops->ctx, tab, addr, n, wr);
}

static mb_ex_t do_read_bits(const mb_slave_ops_t *ops, mb_table_t tab, const uint8_t *pdu, uint16_t pdu_len,
                            uint8_t *out, uint16_t *out_len)
{
    uint16_t addr, n, i, v;
    uint8_t bc;
    mb_ex_t ex;

    if (pdu_len != 5) return MB_EX_ILLEGAL_VALUE;
    addr = get16(&pdu[1]);
    n    = get16(&pdu[3]);
    if (n < 1 || n > MAX_READ_BITS) return MB_EX_ILLEGAL_VALUE;
    if ((ex = check_range(ops, tab, addr, n, 0)) != MB_EX_NONE) return ex;

    bc = (uint8_t)((n + 7) / 8);
    out[0] = bc;
    memset(&out[1], 0, bc);
    for (i = 0; i < n; i++) {
        if ((ex = ops->read(ops->ctx, tab, (uint16_t)(addr + i), &v)) != MB_EX_NONE) return ex;
        if (v) out[1 + i / 8] |= (uint8_t)(1u << (i % 8));
    }
    *out_len = (uint16_t)(1 + bc);
    return MB_EX_NONE;
}

static mb_ex_t do_read_regs(const mb_slave_ops_t *ops, mb_table_t tab, const uint8_t *pdu, uint16_t pdu_len,
                            uint8_t *out, uint16_t *out_len)
{
    uint16_t addr, n, i, v;
    mb_ex_t ex;

    if (pdu_len != 5) return MB_EX_ILLEGAL_VALUE;
    addr = get16(&pdu[1]);
    n    = get16(&pdu[3]);
    if (n < 1 || n > MAX_READ_REGS) return MB_EX_ILLEGAL_VALUE;
    if ((ex = check_range(ops, tab, addr, n, 0)) != MB_EX_NONE) return ex;

    out[0] = (uint8_t)(n * 2);
    for (i = 0; i < n; i++) {
        if ((ex = ops->read(ops->ctx, tab, (uint16_t)(addr + i), &v)) != MB_EX_NONE) return ex;
        put16(&out[1 + 2 * i], v);
    }
    *out_len = (uint16_t)(1 + n * 2);
    return MB_EX_NONE;
}

static mb_ex_t do_write_single(const mb_slave_ops_t *ops, uint8_t fc, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t addr, v;
    mb_table_t tab = (fc == MB_FC_WRITE_COIL) ? MB_TAB_COIL : MB_TAB_HOLDING;
    mb_ex_t ex;

    if (pdu_len != 5) return MB_EX_ILLEGAL_VALUE;
    addr = get16(&pdu[1]);
    v    = get16(&pdu[3]);
    if (fc == MB_FC_WRITE_COIL) {
        if (v != 0x0000 && v != 0xFF00) return MB_EX_ILLEGAL_VALUE;
        v = (v == 0xFF00);
    }
    if ((ex = check_range(ops, tab, addr, 1, 1)) != MB_EX_NONE) return ex;
    return ops->write(ops->ctx, tab, addr, v);
}

static mb_ex_t do_write_multi(const mb_slave_ops_t *ops, uint8_t fc, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t addr, n, i;
    uint8_t bc;
    int coils = (fc == MB_FC_WRITE_COILS);
    mb_table_t tab = coils ? MB_TAB_COIL : MB_TAB_HOLDING;
    mb_ex_t ex;

    if (pdu_len < 6) return MB_EX_ILLEGAL_VALUE;
    addr = get16(&pdu[1]);
    n    = get16(&pdu[3]);
    bc   = pdu[5];
    if (n < 1 || n > (coils ? MAX_WRITE_BITS : MAX_WRITE_REGS)) return MB_EX_ILLEGAL_VALUE;
    if (bc != (coils ? (n + 7) / 8 : n * 2) || pdu_len != 6u + bc) return MB_EX_ILLEGAL_VALUE;
    if ((ex = check_range(ops, tab, addr, n, 1)) != MB_EX_NONE) return ex;

    for (i = 0; i < n; i++) {
        uint16_t v;
        if (coils)
            v = (uint16_t)(((unsigned)pdu[6 + i / 8] >> (i % 8)) & 1u);
        else
            v = get16(&pdu[6 + 2 * i]);
        if ((ex = ops->write(ops->ctx, tab, (uint16_t)(addr + i), v)) != MB_EX_NONE) return ex;
    }
    return MB_EX_NONE;
}

static mb_ex_t do_file_record(const mb_slave_ops_t *ops, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t pos, end;
    uint8_t bc;

    if (!ops->file_record) return MB_EX_ILLEGAL_FUNCTION;
    if (pdu_len < 2) return MB_EX_ILLEGAL_VALUE;
    bc = pdu[1];
    if (bc < FILE_BC_MIN || bc > FILE_BC_MAX || pdu_len != 2u + bc) return MB_EX_ILLEGAL_VALUE;

    /* 一帧里可以有多个子请求，每个：参考类型(1) 文件号(2) 记录号(2) 记录长度(2，单位：寄存器) 数据 */
    pos = 2;
    end = (uint16_t)(2 + bc);
    while (pos < end) {
        uint16_t file_no, rec_no, data_len;
        mb_ex_t ex;
        if (end - pos < 7) return MB_EX_ILLEGAL_VALUE;
        if (pdu[pos] != FILE_REF_TYPE) return MB_EX_ILLEGAL_ADDRESS;
        file_no  = get16(&pdu[pos + 1]);
        rec_no   = get16(&pdu[pos + 3]);
        data_len = (uint16_t)(get16(&pdu[pos + 5]) * 2);
        if (data_len == 0 || pos + 7u + data_len > end) return MB_EX_ILLEGAL_VALUE;
        ex = ops->file_record(ops->ctx, file_no, rec_no, &pdu[pos + 7], data_len);
        if (ex != MB_EX_NONE) return ex;
        pos = (uint16_t)(pos + 7 + data_len);
    }
    return MB_EX_NONE;
}

uint16_t mb_slave_handle(uint8_t my_addr, const mb_slave_ops_t *ops,
                         const uint8_t *req, uint16_t req_len, uint8_t *rsp)
{
    const uint8_t *pdu;
    uint16_t pdu_len, data_len = 0;
    uint8_t fc;
    int broadcast;
    mb_ex_t ex;

    if (req_len < 4 || req_len > MB_ADU_MAX || !crc_ok(req, req_len))
        return 0;                                   /* CRC 错：规范要求静默丢弃 */
    broadcast = (req[0] == MB_ADDR_BROADCAST);
    if (!broadcast && req[0] != my_addr)
        return 0;

    fc      = req[1];
    pdu     = &req[1];
    pdu_len = (uint16_t)(req_len - 3);

    switch (fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
        if (broadcast) return 0;                    /* 广播只能是写操作 */
        ex = do_read_bits(ops, fc == MB_FC_READ_COILS ? MB_TAB_COIL : MB_TAB_DISCRETE,
                          pdu, pdu_len, &rsp[2], &data_len);
        break;
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        if (broadcast) return 0;
        ex = do_read_regs(ops, fc == MB_FC_READ_HOLDING ? MB_TAB_HOLDING : MB_TAB_INPUT,
                          pdu, pdu_len, &rsp[2], &data_len);
        break;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        ex = do_write_single(ops, fc, pdu, pdu_len);
        if (ex == MB_EX_NONE) { memcpy(&rsp[2], &pdu[1], 4); data_len = 4; }    /* 回显地址和数值 */
        break;
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        ex = do_write_multi(ops, fc, pdu, pdu_len);
        if (ex == MB_EX_NONE) { memcpy(&rsp[2], &pdu[1], 4); data_len = 4; }    /* 回显地址和数量 */
        break;
    case MB_FC_WRITE_FILE_RECORD:
        ex = do_file_record(ops, pdu, pdu_len);
        if (ex == MB_EX_NONE) { memcpy(&rsp[2], &pdu[1], (size_t)(pdu_len - 1)); data_len = (uint16_t)(pdu_len - 1); }
        break;
    default:
        ex = MB_EX_ILLEGAL_FUNCTION;
        break;
    }

    if (broadcast)
        return 0;

    rsp[0] = my_addr;
    if (ex != MB_EX_NONE) {
        rsp[1] = (uint8_t)(fc | 0x80);
        rsp[2] = (uint8_t)ex;
        return add_crc(rsp, 3);
    }
    rsp[1] = fc;
    return add_crc(rsp, (uint16_t)(2 + data_len));
}

/* ================================================================ 主机 */

static const uint8_t k_read_fc[MB_TAB_COUNT] = {
    MB_FC_READ_COILS, MB_FC_READ_DISCRETE, MB_FC_READ_INPUT, MB_FC_READ_HOLDING
};

uint16_t mb_build_read(uint8_t *adu, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t count)
{
    int bits = (tab == MB_TAB_COIL || tab == MB_TAB_DISCRETE);

    if (tab >= MB_TAB_COUNT || slave == MB_ADDR_BROADCAST || count == 0 ||
        count > (bits ? MAX_READ_BITS : MAX_READ_REGS))
        return 0;
    adu[0] = slave;
    adu[1] = k_read_fc[tab];
    put16(&adu[2], addr);
    put16(&adu[4], count);
    return add_crc(adu, 6);
}

uint16_t mb_build_write_single(uint8_t *adu, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t value)
{
    if (tab == MB_TAB_COIL) {
        adu[1] = MB_FC_WRITE_COIL;
        value = value ? 0xFF00 : 0x0000;
    } else if (tab == MB_TAB_HOLDING) {
        adu[1] = MB_FC_WRITE_REG;
    } else {
        return 0;                                   /* 1x / 3x 只读 */
    }
    adu[0] = slave;
    put16(&adu[2], addr);
    put16(&adu[4], value);
    return add_crc(adu, 6);
}

uint16_t mb_build_write_regs(uint8_t *adu, uint8_t slave, uint16_t addr, uint16_t count, const uint16_t *values)
{
    uint16_t i;

    if (count == 0 || count > MAX_WRITE_REGS)
        return 0;
    adu[0] = slave;
    adu[1] = MB_FC_WRITE_REGS;
    put16(&adu[2], addr);
    put16(&adu[4], count);
    adu[6] = (uint8_t)(count * 2);
    for (i = 0; i < count; i++)
        put16(&adu[7 + 2 * i], values[i]);
    return add_crc(adu, (uint16_t)(7 + count * 2));
}

uint16_t mb_build_file_record(uint8_t *adu, uint8_t slave, uint16_t file_no, uint16_t record_no,
                              const uint8_t *data, uint16_t len)
{
    uint16_t padded = (uint16_t)((len + 1u) & ~1u); /* 记录长度以寄存器（2 字节）为单位 */

    if (len == 0 || 7u + padded > FILE_BC_MAX)
        return 0;
    adu[0] = slave;
    adu[1] = MB_FC_WRITE_FILE_RECORD;
    adu[2] = (uint8_t)(7 + padded);
    adu[3] = FILE_REF_TYPE;
    put16(&adu[4], file_no);
    put16(&adu[6], record_no);
    put16(&adu[8], (uint16_t)(padded / 2));
    memcpy(&adu[10], data, len);
    if (padded != len)
        adu[10 + len] = 0;
    return add_crc(adu, (uint16_t)(10 + padded));
}

int mb_parse_rsp(const uint8_t *req, uint16_t req_len, const uint8_t *rsp, uint16_t rsp_len,
                 uint16_t *values, uint16_t max_values, mb_ex_t *ex)
{
    uint8_t fc = req[1];
    uint16_t n, i;

    if (ex) *ex = MB_EX_NONE;
    if (rsp_len < 5)                return MB_ERR_FRAME;
    if (!crc_ok(rsp, rsp_len))      return MB_ERR_CRC;
    if (rsp[0] != req[0])           return MB_ERR_FRAME;

    if (rsp[1] == (uint8_t)(fc | 0x80)) {
        if (rsp_len != 5) return MB_ERR_FRAME;
        if (ex) *ex = (mb_ex_t)rsp[2];
        return MB_ERR_EXCEPTION;
    }
    if (rsp[1] != fc)               return MB_ERR_FRAME;

    switch (fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
        n = get16(&req[4]);
        if (rsp[2] != (n + 7) / 8 || rsp_len != 5u + rsp[2]) return MB_ERR_FRAME;
        if (!values || max_values < n) return MB_ERR_PARAM;
        for (i = 0; i < n; i++)
            values[i] = (uint16_t)(((unsigned)rsp[3 + i / 8] >> (i % 8)) & 1u);
        return MB_OK;
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        n = get16(&req[4]);
        if (rsp[2] != n * 2 || rsp_len != 5u + rsp[2]) return MB_ERR_FRAME;
        if (!values || max_values < n) return MB_ERR_PARAM;
        for (i = 0; i < n; i++)
            values[i] = get16(&rsp[3 + 2 * i]);
        return MB_OK;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        /* 单写回显地址和数值，多写回显地址和数量：都是请求的第 2～5 字节 */
        if (rsp_len != 8 || memcmp(&rsp[2], &req[2], 4) != 0) return MB_ERR_FRAME;
        return MB_OK;
    case MB_FC_WRITE_FILE_RECORD:
        if (rsp_len != req_len || memcmp(rsp, req, (size_t)(req_len - 2)) != 0) return MB_ERR_FRAME;
        return MB_OK;
    default:
        return MB_ERR_FRAME;
    }
}
