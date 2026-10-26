/*
 * boot_log.c - Bootloader 的日志：和 App 用同一个 log.h（LOGI / LOGW / LOGE），但实现简单得多
 *
 *   App：日志进队列，低优先级任务慢慢发，不阻塞（第 1 步）
 *   这里：没有任务，直接用 HAL_UART_Transmit 阻塞发送（115200 下一行几毫秒）
 *
 * 格式和 App 一样："[  时间] I tag: 内容\r\n"，时间是 HAL_GetTick()。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdarg.h>
#include <stdio.h>
#include "main.h"
#include "log.h"

#define LINE_MAX 160

static log_level_t s_level = LOG_LVL_INFO;
static uint8_t     s_ready;

void log_init(void)                    { s_ready = 1; }   /* 在 USART3 初始化之后调用 */
void log_set_level(log_level_t level)  { s_level = level; }
uint32_t log_dropped(void)             { return 0; }
uint32_t log_isr_rejected(void)        { return 0; }

static void emit(const char *s, int n)
{
    if (s_ready && n > 0)
        (void)HAL_UART_Transmit(&huart3, (const uint8_t *)s, (uint16_t)n, 100);
}

static int prefix(char *buf, log_level_t level, const char *tag)
{
    static const char k[] = "EWID";
    return snprintf(buf, LINE_MAX, "[%7lu] %c %s: ", (unsigned long)HAL_GetTick(), k[level & 3], tag);
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    char buf[LINE_MAX];
    va_list ap;
    int n;

    if (level > s_level)
        return;
    n = prefix(buf, level, tag);
    va_start(ap, fmt);
    n += vsnprintf(&buf[n], (size_t)(LINE_MAX - 2 - n), fmt, ap);
    va_end(ap);
    if (n > LINE_MAX - 3)
        n = LINE_MAX - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    emit(buf, n);
}

void log_hex(log_level_t level, const char *tag, const char *title, const uint8_t *data, uint16_t len)
{
    char buf[LINE_MAX];
    int n;
    uint16_t i;

    if (level > s_level)
        return;
    n = prefix(buf, level, tag);
    n += snprintf(&buf[n], (size_t)(LINE_MAX - n), "%s %u bytes:", title, (unsigned)len);
    for (i = 0; i < len && n < LINE_MAX - 6; i++)
        n += snprintf(&buf[n], (size_t)(LINE_MAX - n), " %02X", data[i]);
    buf[n++] = '\r';
    buf[n++] = '\n';
    emit(buf, n);
}
