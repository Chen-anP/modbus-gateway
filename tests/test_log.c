/*
 * test_log.c - 在 PC 上测试 log 模块（FreeRTOS 和 HAL 用 tests/stubs 里的替身）
 *
 *   gcc -Wall -Wextra -I tests/stubs -I firmware/central/App firmware/central/App/log.c tests/test_log.c -o test_log && ./test_log
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "main.h"
#include "log.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- 替身的状态 ---- */
UART_HandleTypeDef huart3;
static char     g_uart[4096];          /* 直接发到串口的内容 */
static size_t   g_uart_len;
static int      g_sched = taskSCHEDULER_NOT_STARTED;
static uint32_t g_tick = 1234, g_ipsr;
static uint8_t  g_q[16][200];          /* 队列：最多 16 条，每条按 item 大小复制 */
static unsigned g_q_item, g_q_cap, g_q_count;

void stub_assert_fail(void) { printf("configASSERT failed\n"); g_fail++; }
uint32_t HAL_GetTick(void) { return g_tick; }
uint32_t __get_IPSR(void) { return g_ipsr; }
int HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *d, uint16_t n, uint32_t t)
{ (void)h; (void)t; memcpy(&g_uart[g_uart_len], d, n); g_uart_len += n; g_uart[g_uart_len] = 0; return 0; }
BaseType_t xTaskGetSchedulerState(void) { return g_sched; }
TickType_t xTaskGetTickCount(void) { return g_tick; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s, void *a, UBaseType_t p, void **h)
{ (void)fn; (void)n; (void)s; (void)a; (void)p; (void)h; return pdTRUE; }
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item) { g_q_cap = (unsigned)len; g_q_item = (unsigned)item; return (QueueHandle_t)g_q; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t w)
{ (void)q; (void)w; if (g_q_count >= g_q_cap) return pdFALSE; memcpy(g_q[g_q_count++], item, g_q_item); return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t w) { (void)q; (void)item; (void)w; return pdFALSE; }

/* 取出队列里第 i 条的文本（log_msg_t 的布局：uint16_t len，然后是 text） */
static const char *q_text(unsigned i, uint16_t *len)
{
    static char out[200];
    memcpy(len, g_q[i], sizeof(uint16_t));
    memcpy(out, g_q[i] + 2, *len);
    out[*len] = 0;
    return out;
}

int main(void)
{
    uint16_t len;
    const char *t;

    /* 1. 调度器启动前：直接发送 */
    log_init();
    CHECK(g_q_cap == 16);
    LOGI("boot", "clock %d MHz", 250);
    CHECK(strcmp(g_uart, "[   1234] I boot: clock 250 MHz\r\n") == 0);

    /* 2. 等级过滤：默认 INFO，DEBUG 不输出 */
    g_uart_len = 0; g_uart[0] = 0;
    LOGD("boot", "hidden");
    CHECK(g_uart_len == 0);
    log_set_level(LOG_LVL_DEBUG);
    LOGD("boot", "shown");
    CHECK(strstr(g_uart, "D boot: shown") != NULL);
    log_set_level(LOG_LVL_INFO);

    /* 3. 调度器运行后：进队列，不直接发 */
    g_sched = taskSCHEDULER_RUNNING; g_uart_len = 0; g_tick = 20001;
    LOGW("ch1", "dev %u timeout", 3u);
    CHECK(g_uart_len == 0 && g_q_count == 1);
    t = q_text(0, &len);
    CHECK(strcmp(t, "[  20001] W ch1: dev 3 timeout\r\n") == 0 && len == strlen(t));

    /* 4. 超长内容被截断，但结尾一定是 \r\n，长度不超过 120 */
    char longmsg[300];
    memset(longmsg, 'x', sizeof(longmsg) - 1); longmsg[sizeof(longmsg) - 1] = 0;
    LOGE("big", "%s", longmsg);
    t = q_text(1, &len);
    CHECK(len == 120 && t[118] == '\r' && t[119] == '\n');

    /* 5. 队列满：丢弃并计数，不阻塞 */
    while (g_q_count < g_q_cap) LOGI("fill", "x");
    uint32_t before = log_dropped();
    LOGI("fill", "one too many");
    CHECK(log_dropped() == before + 1);

    /* 6. 中断里调用：拒绝并计数 */
    g_ipsr = 0x2B;
    LOGE("isr", "should not print");
    CHECK(log_isr_rejected() == 1 && log_dropped() == before + 1);
    g_ipsr = 0;

#ifdef TEST_LOG_HEX
    /* 7. log_hex（练习），调度器启动前直接发送，便于检查 */
    g_sched = taskSCHEDULER_NOT_STARTED; g_uart_len = 0; g_uart[0] = 0; g_tick = 5;
    static const uint8_t f[] = {0x01,0x03,0x00,0x00,0x00,0x0A,0xC5,0xCD};
    log_hex(LOG_LVL_INFO, "test", "frame", f, sizeof(f));
    CHECK(strcmp(g_uart, "[      5] I test: frame 8 bytes: 01 03 00 00 00 0A C5 CD\r\n") == 0);
    uint8_t big[20];
    for (int i = 0; i < 20; i++) big[i] = (uint8_t)i;
    g_uart_len = 0;
    log_hex(LOG_LVL_INFO, "t", "big", big, 20);
    CHECK(strstr(g_uart, "big 20 bytes: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\r\n") != NULL);
    CHECK(strstr(g_uart, "I t: 10 11 12 13\r\n") != NULL);
    g_uart_len = 0;
    log_hex(LOG_LVL_INFO, "t", "empty", big, 0);
    CHECK(strcmp(g_uart, "[      5] I t: empty 0 bytes:\r\n") == 0);
    g_uart_len = 0;
    log_hex(LOG_LVL_DEBUG, "t", "filtered", big, 4);
    CHECK(g_uart_len == 0);
#endif

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
