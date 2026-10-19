#include <stdarg.h>
/*
 * test_bus_probe.c - 在 PC 上测试 bus_probe.c 里的 transact()：回显、分包、超时、垃圾数据
 *
 *   gcc -I tests/stubs -I firmware/central/App firmware/central/App/mb_rtu.c tests/test_bus_probe.c -o test_bus_probe
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

UART_HandleTypeDef huart2, huart4;
static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- 脚本化的串口：每次 read 返回下一段数据 ---- */
static const uint8_t *g_chunks[8];
static uint16_t g_chunk_len[8];
static int g_nchunks, g_next;
static uint32_t g_tick;

TickType_t xTaskGetTickCount(void) { return g_tick; }
void vTaskDelay(TickType_t t) { g_tick += t; }
void vTaskDelete(void *h) { (void)h; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p, void **h)
{ (void)fn; (void)n; (void)s; (void)a; (void)p; (void)h; return 1; }
#include "port_uart.h"
int port_uart_init(port_uart_t *p, UART_HandleTypeDef *h, const char *name) { (void)h; p->name = name; return 0; }
int port_uart_send(port_uart_t *p, const uint8_t *d, uint16_t n, uint32_t t) { (void)p; (void)d; (void)n; (void)t; return 0; }
void port_uart_flush(port_uart_t *p) { (void)p; }
uint16_t port_uart_read(port_uart_t *p, uint8_t *buf, uint16_t max, uint32_t t)
{
    (void)p; (void)max;
    if (g_next >= g_nchunks) { g_tick += t; return 0; }      /* 没有更多数据：等满超时 */
    g_tick += 1;
    memcpy(buf, g_chunks[g_next], g_chunk_len[g_next]);
    return g_chunk_len[g_next++];
}
/* 日志替身 */
#include "log.h"
void log_write(log_level_t l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; }
void log_hex(log_level_t l, const char *t, const char *ti, const uint8_t *d, uint16_t n) { (void)l; (void)t; (void)ti; (void)d; (void)n; }
uint32_t log_dropped(void) { return 0; }

#include "../firmware/central/App/bus_probe.c"

static void script(int n, ...)
{
    va_list ap; va_start(ap, n);
    g_nchunks = n; g_next = 0;
    for (int i = 0; i < n; i++) { g_chunks[i] = va_arg(ap, const uint8_t *); g_chunk_len[i] = (uint16_t)va_arg(ap, int); }
    va_end(ap);
}

int main(void)
{
    port_uart_t p; memset(&p, 0, sizeof(p)); p.name = "t";
    uint8_t req[16], rsp[MB_ADU_MAX], ok[16], junk[] = { 0x55, 0xAA };
    uint16_t req_len = mb_build_read(req, 3, MB_TAB_INPUT, 0, 2);   /* 03 04 00 00 00 02 + CRC */
    /* 正常回应：03 04 04 00 FA 01 C2 + CRC（温度 250，湿度 450） */
    const uint8_t body[] = { 0x03, 0x04, 0x04, 0x00, 0xFA, 0x01, 0xC2 };
    uint16_t c = mb_crc16(body, 7);
    memcpy(ok, body, 7); ok[7] = (uint8_t)c; ok[8] = (uint8_t)(c >> 8);
    uint8_t both[32]; memcpy(both, req, req_len); memcpy(both + req_len, ok, 9);

    script(1, ok, 9);
    CHECK(transact(&p, req, req_len, rsp) == 9 && memcmp(rsp, ok, 9) == 0);          /* 1. 正常 */

    script(2, req, (int)req_len, ok, 9);
    CHECK(transact(&p, req, req_len, rsp) == 9 && memcmp(rsp, ok, 9) == 0);          /* 2. 先回显（单独一段）再回应 */

    script(1, both, (int)req_len + 9);
    CHECK(transact(&p, req, req_len, rsp) == 9 && memcmp(rsp, ok, 9) == 0);          /* 3. 回显和回应粘在一段 */

    script(3, ok, 2, ok + 2, 3, ok + 5, 4);
    CHECK(transact(&p, req, req_len, rsp) == 9 && memcmp(rsp, ok, 9) == 0);          /* 4. 回应分三段到达：开头 03 04 和请求一样，不能被当成回显吃掉 */

    script(0);
    CHECK(transact(&p, req, req_len, rsp) == 0);                                      /* 5. 没有回应：超时 */

    script(1, ok, 5);
    CHECK(transact(&p, req, req_len, rsp) == 0);                                      /* 6. 回应只来了一半：超时 */

    script(2, junk, 2, ok, 9);
    int r = transact(&p, req, req_len, rsp);
    /* 7. 前面有垃圾字节 55 AA：AA 最高位是 1，帧组装器按"异常回应 5 字节"切出一帧，
     *    交给 mb_parse_rsp 时 CRC 不对 → probe 打印 bad response。不崩溃、不越界即可 */
    {
        uint16_t v7[2]; mb_ex_t ex7;
        CHECK(r == 5 && mb_parse_rsp(req, req_len, rsp, (uint16_t)r, v7, 2, &ex7) != MB_OK);
    }

    /* 8. 解析：probe 用 mb_parse_rsp 取出 250、450 */
    uint16_t v[2]; mb_ex_t ex;
    CHECK(mb_parse_rsp(req, req_len, ok, 9, v, 2, &ex) == MB_OK && v[0] == 250 && v[1] == 450);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
