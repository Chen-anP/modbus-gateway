/*
 * test_mb_rtu.c - mb_rtu 协议层的 PC 端测试
 *
 *   gcc -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c tests/test_mb_rtu.c -o test_mb_rtu
 *   ./test_mb_rtu
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mb_rtu.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- 测试用从机寄存器表 */
#define TAB_SIZE 100
static uint16_t g_tab[MB_TAB_COUNT][TAB_SIZE];
static int g_fail_addr = -1;                    /* 读写这个地址时模拟设备故障 */
static int g_file_calls;
static uint16_t g_file_no[4], g_file_rec[4], g_file_len[4];
static uint8_t g_file_data[4][256];

static mb_ex_t t_check(void *ctx, mb_table_t tab, uint16_t addr, uint16_t n, int wr)
{
    (void)ctx;
    if (wr && (tab == MB_TAB_DISCRETE || tab == MB_TAB_INPUT)) return MB_EX_ILLEGAL_FUNCTION;
    return ((uint32_t)addr + n <= TAB_SIZE) ? MB_EX_NONE : MB_EX_ILLEGAL_ADDRESS;
}
static mb_ex_t t_read(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *v)
{
    (void)ctx;
    if ((int)addr == g_fail_addr) return MB_EX_DEVICE_FAILURE;
    *v = g_tab[tab][addr];
    return MB_EX_NONE;
}
static mb_ex_t t_write(void *ctx, mb_table_t tab, uint16_t addr, uint16_t v)
{
    (void)ctx;
    if ((int)addr == g_fail_addr) return MB_EX_DEVICE_FAILURE;
    g_tab[tab][addr] = v;
    return MB_EX_NONE;
}
static mb_ex_t t_file(void *ctx, uint16_t file_no, uint16_t rec, const uint8_t *d, uint16_t len)
{
    (void)ctx;
    if (file_no == 0xDEAD) return MB_EX_BUSY;
    if (g_file_calls < 4) {
        g_file_no[g_file_calls] = file_no; g_file_rec[g_file_calls] = rec;
        g_file_len[g_file_calls] = len; memcpy(g_file_data[g_file_calls], d, len);
    }
    g_file_calls++;
    return MB_EX_NONE;
}
static const mb_slave_ops_t OPS = { NULL, t_check, t_read, t_write, t_file };
static const mb_slave_ops_t OPS_NOFILE = { NULL, t_check, t_read, t_write, NULL };

static void reset_tab(void)
{
    for (int t = 0; t < MB_TAB_COUNT; t++)
        for (int i = 0; i < TAB_SIZE; i++)
            g_tab[t][i] = (uint16_t)(t * 1000 + i);
    g_fail_addr = -1;
    g_file_calls = 0;
}

/* 用十六进制字符串造帧，自动补 CRC */
static uint16_t hexframe(uint8_t *out, const char *hex)
{
    uint16_t n = 0;
    unsigned v;
    while (*hex) {
        while (*hex == ' ') hex++;
        if (!*hex || sscanf(hex, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    uint16_t crc = mb_crc16(out, n);
    out[n++] = (uint8_t)crc;
    out[n++] = (uint8_t)(crc >> 8);
    return n;
}

/* 发请求给从机，返回回应长度；*ex 为异常码（没有异常为 0） */
static uint16_t slave(const mb_slave_ops_t *ops, const char *hex, uint8_t *rsp, int *ex)
{
    uint8_t req[MB_ADU_MAX];
    uint16_t n = hexframe(req, hex);
    uint16_t r = mb_slave_handle(1, ops, req, n, rsp);
    *ex = (r == 5 && (rsp[1] & 0x80)) ? rsp[2] : 0;
    return r;
}

/* ---------------------------------------------------------------- 测试 */
static void test_crc(void)
{
    const uint8_t f[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    CHECK(mb_crc16(f, 6) == 0xCDC5);                            /* 帧里是 C5 CD */
    CHECK(mb_crc16((const uint8_t *)"123456789", 9) == 0x4B37); /* CRC-16/MODBUS 标准校验值 */
}

static void test_frame_need(void)
{
    uint8_t b[16] = { 1, 3, 0, 0, 0, 10 };
    CHECK(mb_frame_need(b, 1, 1) == 0);
    CHECK(mb_frame_need(b, 2, 1) == 8);
    b[1] = 0x10; b[6] = 4;
    CHECK(mb_frame_need(b, 6, 1) == 0);                         /* 还没收到字节数 */
    CHECK(mb_frame_need(b, 7, 1) == 13);
    b[1] = 0x15; b[2] = 11;
    CHECK(mb_frame_need(b, 2, 1) == 0);
    CHECK(mb_frame_need(b, 3, 1) == 16);
    b[1] = 0x07;
    CHECK(mb_frame_need(b, 2, 1) < 0);                          /* 不支持的功能码 */
    /* 回应 */
    b[1] = 0x03; b[2] = 20;
    CHECK(mb_frame_need(b, 2, 0) == 0);
    CHECK(mb_frame_need(b, 3, 0) == 25);
    b[1] = 0x83;
    CHECK(mb_frame_need(b, 2, 0) == 5);
    b[1] = 0x06;
    CHECK(mb_frame_need(b, 2, 0) == 8);
    b[1] = 0x15; b[2] = 0xFF;
    CHECK(mb_frame_need(b, 3, 0) < 0);                          /* 5 + 255 超过 256 字节 */
}

static void test_framer(void)
{
    mb_framer_t f;
    uint8_t a[MB_ADU_MAX], two[2 * MB_ADU_MAX];
    uint16_t n = hexframe(a, "01 03 00 00 00 0A"), used = 0;
    int i, got = 0;

    /* 一个字节一个字节地喂 */
    mb_framer_init(&f, 1, 5);
    for (i = 0; i < n; i++)
        got = mb_framer_feed(&f, &a[i], 1, (uint32_t)i, &used);
    CHECK(got == 1 && f.len == n && memcmp(f.buf, a, n) == 0);

    /* 两帧粘在一起：第一次返回第一帧，剩下的再喂 */
    memcpy(two, a, n);
    uint16_t m = hexframe(two + n, "01 06 00 05 12 34");
    mb_framer_reset(&f);
    CHECK(mb_framer_feed(&f, two, (uint16_t)(n + m), 100, &used) == 1 && used == n);
    mb_framer_reset(&f);
    CHECK(mb_framer_feed(&f, two + used, (uint16_t)(n + m - used), 100, &used) == 1 && f.len == m && f.buf[1] == 0x06);

    /* 发了半帧后停了很久：半帧被丢弃，新帧正常收 */
    mb_framer_reset(&f);
    CHECK(mb_framer_feed(&f, a, 4, 200, &used) == 0);
    uint32_t dropped = f.dropped;
    CHECK(mb_framer_feed(&f, a, n, 300, &used) == 1 && f.dropped == dropped + 1 && memcmp(f.buf, a, n) == 0);

    /* 间隔没超过 gap：继续拼成一帧 */
    mb_framer_reset(&f);
    mb_framer_feed(&f, a, 4, 400, &used);
    CHECK(mb_framer_feed(&f, a + 4, (uint16_t)(n - 4), 403, &used) == 1);

    /* 垃圾数据（不认识的功能码）后面跟正常帧 */
    mb_framer_reset(&f);
    uint8_t junk[2] = { 0x01, 0x99 };
    CHECK(mb_framer_feed(&f, junk, 2, 500, &used) == 0 && f.len == 0);
    CHECK(mb_framer_feed(&f, a, n, 501, &used) == 1);
}

static void test_slave_read(void)
{
    uint8_t r[MB_ADU_MAX];
    int ex;
    uint16_t n;

    reset_tab();
    n = slave(&OPS, "01 03 00 02 00 03", r, &ex);               /* 读保持寄存器 2..4 */
    CHECK(n == 11 && ex == 0 && r[2] == 6);
    CHECK(r[3] == (3002 >> 8) && r[4] == (3002 & 0xFF) && r[7] == (3004 >> 8));
    CHECK(mb_crc16(r, n) == 0);                                 /* 整帧含 CRC 再算一遍应为 0 */

    n = slave(&OPS, "01 04 00 00 00 01", r, &ex);               /* 输入寄存器 */
    CHECK(n == 7 && r[3] == (2000 >> 8) && r[4] == (2000 & 0xFF));

    g_tab[MB_TAB_COIL][0] = 1; g_tab[MB_TAB_COIL][3] = 1; g_tab[MB_TAB_COIL][9] = 1;
    for (int i = 1; i < 10; i++) if (i != 3 && i != 9) g_tab[MB_TAB_COIL][i] = 0;
    n = slave(&OPS, "01 01 00 00 00 0A", r, &ex);               /* 10 个线圈打包成 2 字节，低位在前 */
    CHECK(n == 7 && r[2] == 2 && r[3] == 0x09 && r[4] == 0x02);

    slave(&OPS, "01 03 00 00 00 7E", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_VALUE);    /* 126 > 125 */
    slave(&OPS, "01 03 00 00 00 00", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_VALUE);    /* 数量 0 */
    slave(&OPS, "01 03 00 62 00 03", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_ADDRESS);  /* 98+3 > 100 */
    slave(&OPS, "01 03 FF FF 00 02", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_ADDRESS);  /* 越过 0xFFFF */
    slave(&OPS, "01 01 00 00 07 D1", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_VALUE);    /* 2001 个线圈 */
    g_fail_addr = 1;
    slave(&OPS, "01 03 00 00 00 03", r, &ex);  CHECK(ex == MB_EX_DEVICE_FAILURE);
}

static void test_slave_write(void)
{
    uint8_t r[MB_ADU_MAX];
    int ex;
    uint16_t n;

    reset_tab();
    n = slave(&OPS, "01 06 00 05 12 34", r, &ex);
    CHECK(n == 8 && ex == 0 && g_tab[MB_TAB_HOLDING][5] == 0x1234 && r[4] == 0x12 && r[5] == 0x34);

    n = slave(&OPS, "01 05 00 07 FF 00", r, &ex);
    CHECK(n == 8 && g_tab[MB_TAB_COIL][7] == 1);
    slave(&OPS, "01 05 00 07 00 00", r, &ex);
    CHECK(g_tab[MB_TAB_COIL][7] == 0);
    slave(&OPS, "01 05 00 07 12 34", r, &ex);   CHECK(ex == MB_EX_ILLEGAL_VALUE);   /* 只能是 FF00 / 0000 */

    n = slave(&OPS, "01 10 00 0A 00 02 04 AA AA BB BB", r, &ex);
    CHECK(n == 8 && g_tab[MB_TAB_HOLDING][10] == 0xAAAA && g_tab[MB_TAB_HOLDING][11] == 0xBBBB);
    CHECK(r[2] == 0 && r[3] == 0x0A && r[4] == 0 && r[5] == 2);

    /* 多寄存器写一半越界：一个都不能写 */
    g_tab[MB_TAB_HOLDING][99] = 7;
    slave(&OPS, "01 10 00 63 00 02 04 11 11 22 22", r, &ex);
    CHECK(ex == MB_EX_ILLEGAL_ADDRESS && g_tab[MB_TAB_HOLDING][99] == 7);

    slave(&OPS, "01 10 00 00 00 02 03 11 11 22", r, &ex);      CHECK(ex == MB_EX_ILLEGAL_VALUE);  /* 字节数不对 */

    n = slave(&OPS, "01 0F 00 00 00 0A 02 05 02", r, &ex);      /* 10 个线圈：1010000000 0100000000 */
    CHECK(n == 8 && g_tab[MB_TAB_COIL][0] == 1 && g_tab[MB_TAB_COIL][1] == 0 && g_tab[MB_TAB_COIL][2] == 1 && g_tab[MB_TAB_COIL][9] == 1);

    slave(&OPS, "01 07", r, &ex);                               /* 不支持的功能码 */
    CHECK(ex == MB_EX_ILLEGAL_FUNCTION);
}

static void test_slave_addressing(void)
{
    uint8_t req[MB_ADU_MAX], r[MB_ADU_MAX];
    uint16_t n;

    reset_tab();
    n = hexframe(req, "02 03 00 00 00 01");                     /* 不是发给我的 */
    CHECK(mb_slave_handle(1, &OPS, req, n, r) == 0);

    n = hexframe(req, "01 03 00 00 00 01");
    req[n - 1] ^= 0xFF;                                         /* CRC 错 */
    CHECK(mb_slave_handle(1, &OPS, req, n, r) == 0);

    n = hexframe(req, "00 06 00 01 55 55");                     /* 广播写：执行但不回复 */
    CHECK(mb_slave_handle(1, &OPS, req, n, r) == 0 && g_tab[MB_TAB_HOLDING][1] == 0x5555);

    n = hexframe(req, "00 03 00 00 00 01");                     /* 广播读：忽略 */
    CHECK(mb_slave_handle(1, &OPS, req, n, r) == 0);
}

static void test_slave_file(void)
{
    uint8_t r[MB_ADU_MAX], req[MB_ADU_MAX];
    int ex;
    uint16_t n;

    reset_tab();
    n = hexframe(req, "01 15 0B 06 21 03 00 01 00 02 AA BB CC DD");
    CHECK(mb_slave_handle(1, &OPS, req, n, r) == n && memcmp(r, req, n) == 0);   /* 正常回应是原样回显 */
    CHECK(g_file_no[0] == 0x2103 && g_file_rec[0] == 1 && g_file_len[0] == 4 && g_file_data[0][3] == 0xDD);

    /* 一帧两个子请求 */
    reset_tab();
    slave(&OPS, "01 15 14 06 00 01 00 00 00 01 11 22 06 00 02 00 05 00 02 33 44 55 66", r, &ex);
    CHECK(ex == 0 && g_file_calls == 2 && g_file_no[1] == 2 && g_file_rec[1] == 5 && g_file_len[1] == 4);

    slave(&OPS, "01 15 0B 07 00 01 00 01 00 02 AA BB CC DD", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_ADDRESS);  /* 参考类型不是 6 */
    slave(&OPS, "01 15 0B 06 00 01 00 01 00 03 AA BB CC DD", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_VALUE);    /* 记录长度和数据对不上 */
    slave(&OPS, "01 15 0B 06 DE AD 00 01 00 02 AA BB CC DD", r, &ex);  CHECK(ex == MB_EX_BUSY);             /* 上层拒绝 */
    slave(&OPS_NOFILE, "01 15 0B 06 00 01 00 01 00 02 AA BB CC DD", r, &ex);  CHECK(ex == MB_EX_ILLEGAL_FUNCTION);
}

/* 主机组帧 → 从机处理 → 主机解析 */
static int roundtrip(uint8_t *req, uint16_t req_len, uint16_t *vals, uint16_t max, mb_ex_t *ex)
{
    uint8_t rsp[MB_ADU_MAX];
    uint16_t n = mb_slave_handle(1, &OPS, req, req_len, rsp);
    int need = mb_frame_need(rsp, n, 0);
    if (need != n) return 99;                                   /* 回应长度必须和 mb_frame_need 算的一致 */
    return mb_parse_rsp(req, req_len, rsp, n, vals, max, ex);
}

static void test_master(void)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t v[200], n;
    uint16_t w[3] = { 0x1111, 0x2222, 0x3333 };
    uint8_t data[238];
    mb_ex_t ex;

    reset_tab();
    n = mb_build_read(req, 1, MB_TAB_HOLDING, 10, 3);
    CHECK(n == 8 && mb_frame_need(req, n, 1) == 8);
    CHECK(roundtrip(req, n, v, 200, &ex) == MB_OK && v[0] == 3010 && v[2] == 3012);

    n = mb_build_read(req, 1, MB_TAB_DISCRETE, 0, 12);
    g_tab[MB_TAB_DISCRETE][11] = 1;
    CHECK(roundtrip(req, n, v, 200, &ex) == MB_OK && v[11] == 1 && v[0] == 1);   /* 1000 非 0 → 1 */

    n = mb_build_write_single(req, 1, MB_TAB_COIL, 4, 1);
    CHECK(req[4] == 0xFF && roundtrip(req, n, NULL, 0, &ex) == MB_OK && g_tab[MB_TAB_COIL][4] == 1);
    CHECK(mb_build_write_single(req, 1, MB_TAB_INPUT, 0, 1) == 0);               /* 只读表 */

    n = mb_build_write_regs(req, 1, 20, 3, w);
    CHECK(roundtrip(req, n, NULL, 0, &ex) == MB_OK && g_tab[MB_TAB_HOLDING][22] == 0x3333);

    /* 0x15：和 PC 工具组出来的帧逐字节一致 */
    const uint8_t d4[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    n = mb_build_file_record(req, 1, 0x2103, 1, d4, 4);
    const uint8_t expect[] = { 0x01, 0x15, 0x0B, 0x06, 0x21, 0x03, 0x00, 0x01, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD };
    CHECK(n == 16 && memcmp(req, expect, 14) == 0);
    CHECK(roundtrip(req, n, NULL, 0, &ex) == MB_OK);

    n = mb_build_file_record(req, 1, 1, 2, d4, 3);              /* 奇数长度补 0 */
    CHECK(n == 16 && req[9] == 2 && req[13] == 0);
    memset(data, 0x5A, sizeof(data));
    CHECK(mb_build_file_record(req, 1, 1, 1, data, 238) != 0);  /* 规范上限 */
    CHECK(mb_build_file_record(req, 1, 1, 1, data, 239) == 0);
    CHECK(mb_build_file_record(req, 1, 1, 1, data, 224) == 236);/* 本项目每包 224 字节 */

    CHECK(mb_build_read(req, 1, MB_TAB_HOLDING, 0, 126) == 0);
    CHECK(mb_build_read(req, 0, MB_TAB_HOLDING, 0, 1) == 0);    /* 广播不能读 */

    /* 异常回应 */
    n = mb_build_read(req, 1, MB_TAB_HOLDING, 99, 2);
    CHECK(roundtrip(req, n, v, 200, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 回应被破坏 */
    n = mb_build_read(req, 1, MB_TAB_HOLDING, 0, 2);
    uint16_t rn = mb_slave_handle(1, &OPS, req, n, rsp);
    rsp[4] ^= 1;
    CHECK(mb_parse_rsp(req, n, rsp, rn, v, 200, &ex) == MB_ERR_CRC);
    rn = mb_slave_handle(1, &OPS, req, n, rsp);
    CHECK(mb_parse_rsp(req, n, rsp, (uint16_t)(rn - 1), v, 200, &ex) != MB_OK);  /* 截断 */
    rsp[0] = 2; { uint16_t c = mb_crc16(rsp, (uint16_t)(rn - 2)); rsp[rn - 2] = (uint8_t)c; rsp[rn - 1] = (uint8_t)(c >> 8); }
    CHECK(mb_parse_rsp(req, n, rsp, rn, v, 200, &ex) == MB_ERR_FRAME);          /* 地址不对 */
    rn = mb_slave_handle(1, &OPS, req, n, rsp);
    CHECK(mb_parse_rsp(req, n, rsp, rn, v, 1, &ex) == MB_ERR_PARAM);            /* 结果数组太小 */

    /* 0x15 回显内容不一致（对应 libmodbus 那个未初始化变量的问题） */
    n = mb_build_file_record(req, 1, 1, 1, d4, 4);
    rn = mb_slave_handle(1, &OPS, req, n, rsp);
    rsp[12] ^= 0xFF; { uint16_t c = mb_crc16(rsp, (uint16_t)(rn - 2)); rsp[rn - 2] = (uint8_t)c; rsp[rn - 1] = (uint8_t)(c >> 8); }
    CHECK(mb_parse_rsp(req, n, rsp, rn, NULL, 0, &ex) == MB_ERR_FRAME);
}

int main(void)
{
    test_crc();
    test_frame_need();
    test_framer();
    test_slave_read();
    test_slave_write();
    test_slave_addressing();
    test_slave_file();
    test_master();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
