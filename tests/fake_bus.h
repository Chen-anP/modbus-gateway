/*
 * fake_bus.h - PC 测试用的"假总线"：实现 mb_io_t，给 test_mb_master.c 和 test_point_sync.c 共用
 *
 * 两种用法：
 *   1. 脚本模式 fb_script()：预先排好每次 read 返回哪一段字节（测回显、分包、垃圾数据）
 *   2. 从机模式 fb_add_slave()：挂几个模拟从机（用 mb_rtu.c 的 mb_slave_handle 生成真实回应），
 *      可以随时"拔掉"（online = 0）或者让下一次回应 CRC 出错
 *
 * 时间是假的：g_now 只在 read 等待、delay 时往前走，所以测试跑得很快，结果也每次一样。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FAKE_BUS_H
#define FAKE_BUS_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mb_rtu.h"
#include "mb_master.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- 时间 */
static uint32_t g_now;
static uint32_t fb_now(void) { return g_now; }
static void fb_delay(uint32_t ms) { g_now += ms; }

/* ---------------------------------------------------------------- 模拟从机 */
typedef struct {
    uint8_t  addr;
    uint8_t  online;        /* 0 = 拔掉了，不回应 */
    uint8_t  corrupt_next;  /* >0：接下来几次回应把 CRC 弄坏 */
    uint16_t coil[8];       /* 0x 线圈 */
    uint16_t disc[8];       /* 1x 离散输入 */
    uint16_t input[8];      /* 3x 输入寄存器 */
    uint16_t holding[8];    /* 4x 保持寄存器 */
    uint32_t writes;        /* 收到几次写请求 */
    uint32_t requests;      /* 收到几次发给自己的请求 */
} fb_slave_t;

static fb_slave_t g_slaves[4];
static int g_nslaves;

static mb_ex_t sl_check(void *ctx, mb_table_t tab, uint16_t addr, uint16_t count, int is_write)
{
    (void)ctx; (void)tab; (void)is_write;
    return (addr + count <= 8) ? MB_EX_NONE : MB_EX_ILLEGAL_ADDRESS;
}
static uint16_t *sl_table(fb_slave_t *s, mb_table_t tab)
{
    switch (tab) {
    case MB_TAB_COIL:     return s->coil;
    case MB_TAB_DISCRETE: return s->disc;
    case MB_TAB_INPUT:    return s->input;
    default:              return s->holding;
    }
}
static mb_ex_t sl_read(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *value)
{
    *value = sl_table((fb_slave_t *)ctx, tab)[addr];
    return MB_EX_NONE;
}
static mb_ex_t sl_write(void *ctx, mb_table_t tab, uint16_t addr, uint16_t value)
{
    fb_slave_t *s = (fb_slave_t *)ctx;
    sl_table(s, tab)[addr] = (tab == MB_TAB_COIL) ? (value ? 1u : 0u) : value;
    s->writes++;
    return MB_EX_NONE;
}

static fb_slave_t *fb_add_slave(uint8_t addr)
{
    fb_slave_t *s = &g_slaves[g_nslaves++];
    memset(s, 0, sizeof(*s));
    s->addr = addr;
    s->online = 1;
    return s;
}

/* ---------------------------------------------------------------- 接收队列 / 脚本 */
static uint8_t  g_rxq[1024];
static uint16_t g_rxq_len;
static const uint8_t *g_chunks[8];
static uint16_t g_chunk_len[8];
static int g_nchunks, g_next, g_script_mode;
static int g_echo;              /* 1 = 收发器回显：先把请求原样收回来（整条总线一份） */
static int g_send_fail;         /* >0：接下来几次 send 失败 */
static uint32_t g_sends;        /* 一共发了几帧 */
static uint32_t g_wait_ms;      /* read 里因为没数据而等掉的时间（掉线设备拖慢总线的指标） */

static void fb_reset(void)
{
    g_nslaves = 0; g_rxq_len = 0; g_nchunks = g_next = 0; g_script_mode = 0;
    g_echo = 0; g_send_fail = 0; g_sends = 0; g_wait_ms = 0;
}

static inline void fb_script(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    g_script_mode = 1; g_nchunks = n; g_next = 0;
    for (int i = 0; i < n; i++) {
        g_chunks[i] = va_arg(ap, const uint8_t *);
        g_chunk_len[i] = (uint16_t)va_arg(ap, int);
    }
    va_end(ap);
}

static void rxq_push(const uint8_t *d, uint16_t n)
{
    memcpy(&g_rxq[g_rxq_len], d, n);
    g_rxq_len = (uint16_t)(g_rxq_len + n);
}

static int fb_send(void *ctx, const uint8_t *d, uint16_t n, uint32_t timeout_ms)
{
    uint8_t rsp[MB_ADU_MAX];
    (void)ctx; (void)timeout_ms;

    if (g_send_fail > 0) { g_send_fail--; return -1; }
    g_sends++;
    g_now += 1;                                         /* 115200 下 8 字节约 0.7 ms */
    if (g_script_mode)
        return 0;
    if (g_echo)
        rxq_push(d, n);
    for (int i = 0; i < g_nslaves; i++) {
        fb_slave_t *s = &g_slaves[i];
        mb_slave_ops_t ops = { s, sl_check, sl_read, sl_write, NULL };
        uint16_t len;

        if (!s->online || (d[0] != s->addr && d[0] != MB_ADDR_BROADCAST))
            continue;
        if (d[0] == s->addr)
            s->requests++;
        len = mb_slave_handle(s->addr, &ops, d, n, rsp);
        if (len == 0)
            continue;
        if (s->corrupt_next > 0) {
            s->corrupt_next--;
            rsp[len - 1] ^= 0xFF;
        }
        rxq_push(rsp, len);
    }
    return 0;
}

static uint16_t fb_read(void *ctx, uint8_t *buf, uint16_t max, uint32_t timeout_ms)
{
    (void)ctx;
    if (g_script_mode) {
        if (g_next >= g_nchunks) { g_now += timeout_ms; g_wait_ms += timeout_ms; return 0; }
        g_now += 1;
        memcpy(buf, g_chunks[g_next], g_chunk_len[g_next]);
        return g_chunk_len[g_next++];
    }
    if (g_rxq_len == 0) { g_now += timeout_ms; g_wait_ms += timeout_ms; return 0; }
    {
        uint16_t n = g_rxq_len < max ? g_rxq_len : max;
        g_now += 1;
        memcpy(buf, g_rxq, n);
        memmove(g_rxq, &g_rxq[n], (size_t)(g_rxq_len - n));
        g_rxq_len = (uint16_t)(g_rxq_len - n);
        return n;
    }
}

static void fb_flush(void *ctx) { (void)ctx; g_rxq_len = 0; }

static const mb_io_t g_io = { NULL, "t", fb_send, fb_read, fb_flush, fb_now, fb_delay };

/* ---------------------------------------------------------------- 日志替身：记下 WARN 以上的条数，VERBOSE 时打印 */
#include "log.h"
static int g_log_warn, g_log_info;
static int g_verbose;
void log_write(log_level_t l, const char *t, const char *f, ...)
{
    va_list ap;
    if (l <= LOG_LVL_WARN) g_log_warn++;
    else if (l == LOG_LVL_INFO) g_log_info++;
    if (!g_verbose || l > LOG_LVL_INFO) return;
    printf("    [%6lu] %s: ", (unsigned long)g_now, t);
    va_start(ap, f); vprintf(f, ap); va_end(ap);
    printf("\n");
}
void log_hex(log_level_t l, const char *t, const char *ti, const uint8_t *d, uint16_t n)
{ (void)l; (void)t; (void)ti; (void)d; (void)n; }

#endif /* FAKE_BUS_H */
