/*
 * poll_task.c - 把 point_sync 接到 FreeRTOS 和串口上
 *
 * 这个文件只做"接线"：
 *   1. 把 port_uart 的函数包装成 mb_io_t（mb_master 只认这组函数指针，所以 PC 上能换成模拟串口）
 *   2. 每个通道一个任务：循环调用 point_sync_step()，每 10 秒打印一次汇总
 * 每个通道读哪些设备、哪些寄存器，由点表决定（gw_data.c 加载）。
 *
 * 为什么每个通道一个任务：两条总线互不影响。ch1 上某个设备超时等 100 ms 的时候，ch2 照常同步。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "log.h"
#include "port_uart.h"
#include "gw_data.h"
#include "wdog.h"
#include "poll_task.h"

#define RSP_TIMEOUT_MS   100u   /* 等回应最多 100 ms（实测往返 1~2 ms，留足余量） */
#define RETRIES          2u     /* 失败后再试 2 次，一共最多 3 次 */
#define LOOP_SLEEP_MS    50u    /* 每一轮之后睡 50 ms，给低优先级任务留时间 */
#define REPORT_MS        10000u
#define POLL_STACK_WORDS 768u   /* 第 3 步实测用了 290 字；point_sync 多了一个 16 个值的读缓冲，余量足够 */

/* ---------------------------------------------------------------- mb_io_t 适配 */

static int io_send(void *ctx, const uint8_t *d, uint16_t n, uint32_t timeout_ms)
{
    return port_uart_send((port_uart_t *)ctx, d, n, timeout_ms);
}

static uint16_t io_read(void *ctx, uint8_t *buf, uint16_t max, uint32_t timeout_ms)
{
    return port_uart_read((port_uart_t *)ctx, buf, max, timeout_ms);
}

static void io_flush(void *ctx)
{
    port_uart_flush((port_uart_t *)ctx);
}

static uint32_t io_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void io_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

/* ---------------------------------------------------------------- 每个通道的全部状态 */

typedef struct {
    uint8_t             ch;         /* 1、2 */
    uint32_t            table_gen;  /* 现在用的是哪个版本的点表 */
    UART_HandleTypeDef *huart;
    port_uart_t         port;
    mb_io_t             io;
    mb_master_t         mb;
    point_sync_t        ps;
    uint8_t             ready;
} channel_t;

static channel_t s_ch[2] = {
    { .ch = 1, .huart = &huart2, .io = { .name = "ch1" } },
    { .ch = 2, .huart = &huart4, .io = { .name = "ch2" } },
};

static void poll_task(void *arg)
{
    channel_t *c = (channel_t *)arg;
    uint32_t next_report;

    c->io.ctx      = &c->port;
    c->io.send     = io_send;
    c->io.read     = io_read;
    c->io.flush    = io_flush;
    c->io.now_ms   = io_now_ms;
    c->io.delay_ms = io_delay_ms;

    if (port_uart_init(&c->port, c->huart, c->io.name) != 0) {
        LOGE(c->io.name, "port init failed");
        vTaskDelete(NULL);
        return;
    }
    mb_master_init(&c->mb, &c->io, RSP_TIMEOUT_MS, RETRIES);
    c->mb.echo = MB_ECHO_NONE;      /* 中控的收发器不回显（实测见 docs/debug-log.md） */

    next_report = io_now_ms() + REPORT_MS;
    for (;;) {
        uint32_t gen = gw_round_begin(c->ch);   /* 一轮开始：换表的时候会在这里等 */

        if (gen != c->table_gen) {              /* 点表换过了（开机第一轮也算）：按新表重新找设备 */
            c->table_gen = gen;
            point_sync_init(&c->ps, c->ch, &c->mb, gw_rs(), gw_points());   /* 设备数在解析点表时检查过 */
            c->ready = 1;
            LOGI(c->io.name, "sync start: %u device(s), table #%lu", (unsigned)c->ps.ndev, (unsigned long)gen);
        }
        point_sync_step(&c->ps);
        gw_round_end(c->ch);
        wdog_beat((uint32_t)(c->ch - 1u));      /* 这一轮跑完了：向看门狗报到（第 8 步） */
        if (wdog_test_should_hang((uint32_t)(c->ch - 1u))) {
            LOGW(c->io.name, "test: task hangs now (watchdog should reset in a few seconds)");
            for (;;)
                vTaskDelay(pdMS_TO_TICKS(1000));   /* 卡在这里不再报到：模拟死锁 */
        }

        if ((int32_t)(io_now_ms() - next_report) >= 0) {
            next_report += REPORT_MS;
            point_sync_report(&c->ps);
            LOGI(c->io.name, "port tx %lu rx %lu ev %lu err %lu ovf %lu | stack left %lu words",
                 (unsigned long)c->port.tx_bytes, (unsigned long)c->port.rx_bytes,
                 (unsigned long)c->port.rx_events, (unsigned long)c->port.rx_errors,
                 (unsigned long)c->port.rx_overflow,
                 (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_SLEEP_MS));
    }
}

void poll_start(void)
{
    BaseType_t r1, r2;

    gw_data_init();
    wdog_start();                           /* 第 8 步：复位原因、启动次数、看门狗任务 */
    r1 = xTaskCreate(poll_task, "ch1", POLL_STACK_WORDS, &s_ch[0], tskIDLE_PRIORITY + 3, NULL);
    r2 = xTaskCreate(poll_task, "ch2", POLL_STACK_WORDS, &s_ch[1], tskIDLE_PRIORITY + 3, NULL);
    configASSERT(r1 == pdPASS && r2 == pdPASS);
    (void)r1; (void)r2;
}

mb_master_t *poll_master(uint8_t ch)
{
    if (ch >= 2 || !s_ch[ch].ready)
        return NULL;
    return &s_ch[ch].mb;
}

const point_sync_t *poll_get(uint8_t ch)
{
    if (ch >= 2 || !s_ch[ch].ready)
        return NULL;
    return &s_ch[ch].ps;
}
