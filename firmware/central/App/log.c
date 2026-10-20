/*
 * log.c - 带等级的日志模块，说明见 log.h
 *
 * 数据流：
 *   任意任务 LOGI(...) → vsnprintf 格式化到栈上的临时数组
 *                      → xQueueSend(不等待) 复制进队列（满了就丢弃、计数）
 *   日志任务（最低优先级之一） ← xQueueReceive(一直等) ← 队列
 *                      → HAL_UART_Transmit 阻塞发送到 USART3
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "main.h"
#include "usart.h"          /* huart3 */
#include "log.h"

#define LOG_LINE_MAX      120u              /* 一条日志最多多少字节（含 "\r\n"），超出的部分截断 */
#define LOG_QUEUE_DEPTH   16u               /* 队列能存多少条：16 × 120 = 1920 字节 */
#define LOG_TASK_STACK    (256u)            /* 单位是"字"（4 字节），即 1 KB */
#define LOG_TASK_PRIO     (tskIDLE_PRIORITY + 1)   /* 只比空闲任务高：有空才打印 */
#define LOG_UART          (&huart3)
#define LOG_TX_TIMEOUT_MS 100u

/* 队列里的一条消息：长度 + 内容（定长，入队时整块复制） */
typedef struct {
    uint16_t len;
    char     text[LOG_LINE_MAX];
} log_msg_t;

static QueueHandle_t     s_queue;
static volatile uint8_t  s_level = LOG_LVL_INFO;
static volatile uint32_t s_dropped;
static volatile uint32_t s_isr_rejected;

static const char k_level_char[] = { 'E', 'W', 'I', 'D' };

/* 当前是否在中断里：IPSR 寄存器非 0 表示正在执行某个异常/中断 */
static int in_isr(void)
{
    return __get_IPSR() != 0u;
}

static void uart_send(const char *s, uint16_t len)
{
    HAL_UART_Transmit(LOG_UART, (const uint8_t *)s, len, LOG_TX_TIMEOUT_MS);
}

static void log_task(void *arg)
{
    log_msg_t msg;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE)
            uart_send(msg.text, msg.len);
    }
}

void log_init(void)
{
    s_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_msg_t));
    configASSERT(s_queue != NULL);

    if (xTaskCreate(log_task, "log", LOG_TASK_STACK, NULL, LOG_TASK_PRIO, NULL) != pdPASS)
        configASSERT(0);
}

void log_set_level(log_level_t level)
{
    s_level = (uint8_t)level;
}

uint32_t log_dropped(void)      { return s_dropped; }
uint32_t log_isr_rejected(void) { return s_isr_rejected; }

/* 把一条已经格式化好的文本交出去：调度器跑起来以后进队列，之前直接发 */
static void log_emit(const char *text, uint16_t len)
{
    log_msg_t msg;

    if (s_queue == NULL || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        uart_send(text, len);               /* 初始化阶段只有一个执行流，直接阻塞发送是安全的 */
        return;
    }
    msg.len = len;
    memcpy(msg.text, text, len);
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
        s_dropped++;                        /* 不等待：宁可丢日志，也不能卡住通信任务 */
}

/* 生成 "[  12345] I tag: " 前缀，返回写入的长度 */
static int format_prefix(char *buf, size_t size, log_level_t level, const char *tag)
{
    uint32_t ms = (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
                ? HAL_GetTick() : (uint32_t)xTaskGetTickCount();   /* 节拍 1 ms，两者都是毫秒 */
    return snprintf(buf, size, "[%7lu] %c %s: ", (unsigned long)ms, k_level_char[level], tag);
}

/* 末尾补 "\r\n"；如果内容太长被截断，保证换行符还在 */
static uint16_t finish_line(char *buf, int n)
{
    if (n < 0) n = 0;
    if (n > (int)LOG_LINE_MAX - 2) n = (int)LOG_LINE_MAX - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return (uint16_t)n;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list ap;
    int n;

    if ((uint8_t)level > s_level)
        return;
    if (in_isr()) {                         /* 规则：中断里不打日志，只计数 */
        s_isr_rejected++;
        return;
    }

    n = format_prefix(buf, sizeof(buf), level, tag);
    if (n < 0 || n >= (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    va_start(ap, fmt);
    n += vsnprintf(&buf[n], sizeof(buf) - 2 - (size_t)n, fmt, ap);   /* 给 "\r\n" 留 2 字节 */
    va_end(ap);

    log_emit(buf, finish_line(buf, n));
}

/* ---- 练习：在这里实现 log_hex ----
 * 要求：
 *   1. 等级过滤、中断检查和 log_write 一样
 *   2. 第一行格式："[时间] D tag: <title> <len> bytes: 01 03 00 ..."
 *   3. 每行最多 16 个字节；超过 16 个字节时，后面的字节另起一行，前缀相同
 *   4. 不能越过 LOG_LINE_MAX（算一下：前缀最长多少？16 字节 × 3 个字符 = 48）
 *   5. len 为 0 时只打印标题
 * 提示：可以先 format_prefix() 得到前缀长度，再用 snprintf(&buf[n], ..., "%02X ", data[i]) 逐个追加，
 *       最后用 finish_line() 和 log_emit() 输出。
 * 写完后在默认任务里调用测试：
 *   static const uint8_t f[] = {0x01,0x03,0x00,0x00,0x00,0x0A,0xC5,0xCD};
 *   log_hex(LOG_LVL_INFO, "test", "frame", f, sizeof(f));
 */
void log_hex(log_level_t level, const char *tag, const char *title, const uint8_t *data, uint16_t len)
{
    /* TODO：你来写 */
    char buf[LOG_LINE_MAX];
    int n;
    uint16_t i = 0;

    if ((uint8_t)level > s_level)
        return;
    if (in_isr()) {
        s_isr_rejected++;
        return;
    }

    n = format_prefix(buf, sizeof(buf), level, tag);
    n += snprintf(&buf[n], sizeof(buf) - 2 - (size_t)n, "%s %u bytes:", title, (unsigned)len);

    for (;;) {
        /* 本行最多再放 16 个字节 */
        uint16_t end = (uint16_t)(((uint32_t)len - i > 16u) ? (uint32_t)i + 16u : len);
        for (; i < end && n < (int)sizeof(buf) - 2; i++)
            n += snprintf(&buf[n], sizeof(buf) - 2 - (size_t)n, " %02X", data[i]);
        log_emit(buf, finish_line(buf, n));
        if (i >= len)
            break;
        n = format_prefix(buf, sizeof(buf), level, tag);   /* 下一行：只有前缀，没有标题 */
        if (n > 0 && buf[n - 1] == ' ')
            n--;                                            /* 前缀以 ": " 结尾，字节前面自带空格，去掉一个 */
    }
}
