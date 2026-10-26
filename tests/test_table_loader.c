/*
 * test_table_loader.c - 点表下发：用真实的 0x15 请求帧走 mb_slave_handle()，收齐后换表
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c firmware/central/App/crc32.c
 *       firmware/central/App/file_rx.c firmware/central/App/point_table.c firmware/central/App/default_points.c
 *       firmware/central/App/sys_regs.c firmware/central/App/reg_space.c firmware/central/App/table_loader.c
 *       tests/test_table_loader.c
 *
 * 和板子上的区别只有一处：板子上换表要等两个通道停下来（gw_apply_table），这里直接换。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "mb_rtu.h"
#include "crc32.h"
#include "point_table.h"
#include "default_points.h"
#include "reg_space.h"
#include "table_loader.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static point_table_t pt;
static sys_regs_t sys;
static reg_space_t rs;
static table_loader_t tl;
static mb_slave_ops_t ops;

/* 和 gw_file_record 一样：交给 table_loader，出错记进 LAST_ERR */
static mb_ex_t file_cb(void *ctx, uint16_t file_no, uint16_t rec, const uint8_t *d, uint16_t len)
{
    mb_ex_t ex;
    (void)ctx;
    ex = table_loader_record(&tl, file_no, rec, d, len);
    if (tl.last_err)
        sys.last_err = tl.last_err;
    else if (tl.ready)
        sys.last_err = 0;
    return ex;
}

/* 和 gw_apply_table 一样（不用等通道） */
static void apply(void)
{
    if (!tl.ready) return;
    memcpy(&pt, &tl.tmp, sizeof(pt));
    tl.ready = 0;
    reg_space_load(&rs, &pt);
}

/* 发一个 0x15 子请求，返回结果（MB_OK / MB_ERR_EXCEPTION），*ex 是异常码 */
static int send_rec(uint16_t file_no, uint16_t rec, const uint8_t *d, uint16_t len, mb_ex_t *ex)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t n = mb_build_file_record(req, 1, file_no, rec, d, len);
    uint16_t r = mb_slave_handle(1, &ops, req, n, rsp);
    return r ? mb_parse_rsp(req, n, rsp, r, NULL, 0, ex) : 99;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* 和上位机工具一样：文件头 + 每 224 字节一包 */
static int send_file(uint16_t file_no, const uint8_t *data, uint32_t len, uint32_t crc, mb_ex_t *ex)
{
    uint8_t hdr[FILE_HDR_SIZE] = { 0 };
    uint16_t rec = 1;
    int rc;

    put32(&hdr[0], 1); put32(&hdr[4], len); put32(&hdr[12], crc);
    memcpy(&hdr[16], "points.bin", 10);
    if ((rc = send_rec(file_no, 0, hdr, FILE_HDR_SIZE, ex)) != MB_OK)
        return rc;
    for (uint32_t off = 0; off < len; off += FILE_REC_DATA) {
        uint16_t n = (uint16_t)((len - off) < FILE_REC_DATA ? (len - off) : FILE_REC_DATA);
        if ((rc = send_rec(file_no, rec++, &data[off], n, ex)) != MB_OK)
            return rc;
    }
    return MB_OK;
}

static uint16_t up_read1(mb_table_t tab, uint16_t addr, mb_ex_t *ex)
{
    uint8_t req[16], rsp[MB_ADU_MAX];
    uint16_t v[1] = { 0xEEEE }, len = mb_build_read(req, 1, tab, addr, 1), n;
    n = mb_slave_handle(1, &ops, req, len, rsp);
    *ex = MB_EX_NONE;
    if (n == 0 || mb_parse_rsp(req, len, rsp, n, v, 1, ex) != MB_OK)
        return 0xEEEE;
    return v[0];
}

int main(void)
{
    static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE];
    uint32_t len;
    mb_ex_t ex;

    len = default_points(buf, sizeof(buf));
    point_table_parse(&pt, buf, len, NULL);
    sys_regs_init(&sys, 6, NULL);
    reg_space_init(&rs, &sys, NULL, NULL);
    reg_space_load(&rs, &pt);
    reg_space_ops(&rs, &ops);
    ops.file_record = file_cb;
    table_loader_init(&tl);

    /* 1. 一张新表：只映射温湿度（中控 3x 0～1 → ch2 地址 3 的 3x 0～1）+ 开关量按键（1x 5～7） */
    {
        const pt_point_t p[] = {
            { MB_TAB_INPUT, 2, 3, 0, 0 }, { MB_TAB_INPUT, 2, 3, 1, 1 },
            { MB_TAB_DISCRETE, 1, 1, 5, 0 }, { MB_TAB_DISCRETE, 1, 1, 6, 1 }, { MB_TAB_DISCRETE, 1, 1, 7, 2 },
        };
        uint8_t f[64];
        uint32_t n = point_table_encode(p, 5, f, sizeof(f));
        CHECK(send_file(FILE_NO_POINT_TABLE, f, n, crc32_update(0, f, n), &ex) == MB_OK);
        CHECK(tl.ready == 1 && tl.tmp.n == 5 && tl.tmp.nblocks == 2);
        /* 换上之前，旧表照常：3x 20 有映射、3x 0 没有 */
        up_read1(MB_TAB_INPUT, 20, &ex);  CHECK(ex == MB_EX_NONE);
        up_read1(MB_TAB_INPUT, 0, &ex);   CHECK(ex == MB_EX_ILLEGAL_ADDRESS);
        apply();
        /* 换上之后：新地址能读、旧地址没了、POINT_CNT = 5 */
        up_read1(MB_TAB_INPUT, 0, &ex);   CHECK(ex == MB_EX_NONE);
        up_read1(MB_TAB_DISCRETE, 7, &ex); CHECK(ex == MB_EX_NONE);
        up_read1(MB_TAB_INPUT, 20, &ex);  CHECK(ex == MB_EX_ILLEGAL_ADDRESS);
        CHECK(up_read1(MB_TAB_HOLDING, 0xF003, &ex) == 5 && sys.last_err == 0);
    }

    /* 2. 内容不合法（重复的中控地址）：最后一包回异常 04，LAST_ERR = 0x0107，正在用的表不变 */
    {
        const pt_point_t p[] = { { MB_TAB_COIL, 1, 1, 3, 0 }, { MB_TAB_COIL, 1, 1, 3, 1 } };
        uint8_t f[32];
        uint32_t n = point_table_encode(p, 2, f, sizeof(f));
        CHECK(send_file(FILE_NO_POINT_TABLE, f, n, crc32_update(0, f, n), &ex) == MB_ERR_EXCEPTION && ex == MB_EX_DEVICE_FAILURE);
        CHECK(tl.ready == 0 && sys.last_err == (0x0100 | PT_ERR_DUP) && tl.bad_point == 1);
        apply();                                              /* 没有新表：什么都不做 */
        CHECK(up_read1(MB_TAB_HOLDING, 0xF003, &ex) == 5);
    }

    /* 3. CRC 不对：异常 04，LAST_ERR = 0x0305 */
    {
        uint8_t f[16];
        const pt_point_t p[] = { { MB_TAB_COIL, 1, 1, 3, 0 } };
        uint32_t n = point_table_encode(p, 1, f, sizeof(f));
        CHECK(send_file(FILE_NO_POINT_TABLE, f, n, 0xDEADBEEFu, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_DEVICE_FAILURE);
        CHECK(sys.last_err == (0x0300 | FRX_ERR_CRC) && tl.ready == 0);
    }

    /* 4. 别的 file_no（固件，以后再做）：异常 02 */
    CHECK(send_rec(0x1000, 0, buf, 32, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 5. 设备太多：ch1 上 9 个设备 → 拒绝（每个通道最多 8 个） */
    {
        pt_point_t p[9];
        uint8_t f[80];
        uint32_t n;
        for (uint8_t i = 0; i < 9; i++) { pt_point_t q = { MB_TAB_COIL, 1, (uint8_t)(i + 1), i, 0 }; p[i] = q; }
        n = point_table_encode(p, 9, f, sizeof(f));
        CHECK(send_file(FILE_NO_POINT_TABLE, f, n, crc32_update(0, f, n), &ex) == MB_ERR_EXCEPTION);
        CHECK(sys.last_err == (0x0100 | PT_ERR_DEVS));
    }

    /* 6. 128 个点（满）：1024 字节 = 4 包 224 + 1 包 128；换上后 POINT_CNT = 128 */
    {
        pt_point_t p[PT_MAX_POINTS];
        uint32_t n;
        for (uint16_t i = 0; i < PT_MAX_POINTS; i++) { pt_point_t q = { MB_TAB_HOLDING, (uint8_t)(1 + i % 2), 5, i, i }; p[i] = q; }
        n = point_table_encode(p, PT_MAX_POINTS, buf, sizeof(buf));
        CHECK(n == 1024);
        CHECK(sys.last_err != 0);                              /* 上一个用例留下的错误 */
        CHECK(send_file(FILE_NO_POINT_TABLE, buf, n, crc32_update(0, buf, n), &ex) == MB_OK && tl.ready);
        CHECK(sys.last_err == 0);                              /* 收到合法的新表，错误清掉 */
        apply();
        CHECK(up_read1(MB_TAB_HOLDING, 0xF003, &ex) == 128);
    }

    /* 7. 换表后，之前的待写入被清掉（新表里同一个下标是另一个点了） */
    {
        uint16_t val;
        uint8_t req[16], rsp[MB_ADU_MAX];
        uint16_t n = mb_build_write_single(req, 1, MB_TAB_HOLDING, 3, 99);
        mb_slave_handle(1, &ops, req, n, rsp);
        CHECK(reg_space_next_pending(&rs, 2, 0, &val) >= 0 || reg_space_next_pending(&rs, 1, 0, &val) >= 0);
        reg_space_load(&rs, &pt);
        CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1 && reg_space_next_pending(&rs, 2, 0, &val) == -1);
    }

    printf("table_loader: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
