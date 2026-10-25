/*
 * poll_task.c - 第 3 步：把 poll 模块接到 FreeRTOS 和串口上
 *
 * 这个文件只做"接线"：
 *   1. 把 port_uart 的函数包装成 mb_io_t（mb_master 只认这组函数指针，所以 PC 上能换成模拟串口）
 *   2. 写死每个通道上有哪些设备（第 5 步换成点表）
 *   3. 每个通道一个任务：循环调用 poll_ch_step()，每 10 秒打印一次汇总
 *
 * 为什么每个通道一个任务：两条总线互不影响。ch1 上某个设备超时等 100 ms 的时候，ch2 照常轮询。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "log.h"
#include "port_uart.h"
#include "poll_task.h"

#define RSP_TIMEOUT_MS  100u    /* 等回应最多 100 ms（实测往返 1~2 ms，留足余量） */
#define RETRIES         2u      /* 失败后再试 2 次，一共最多 3 次 */
#define LOOP_SLEEP_MS   50u     /* 每扫一遍睡 50 ms：周期的精度是 50 ms，也给低优先级任务留时间 */
#define REPORT_MS       10000u  /* 汇总间隔 */
#define POLL_STACK_WORDS 768u

/* ---------------------------------------------------------------- 设备表 */

/* ch1（USART2）：开关量 + 环境监测 */
static const poll_dev_cfg_t k_ch1_devs[] = {
    /* 名字      地址  表               起始 个数 周期 ms */
    { "switch", 1, MB_TAB_DISCRETE, 0, 3, 200  },  /* 3 路开关量：变化要快点看到 */
    { "env",    2, MB_TAB_INPUT,    0, 2, 500  },  /* 2 路 ADC */
};

/* ch2（UART4）：温湿度 */
static const poll_dev_cfg_t k_ch2_devs[] = {
    { "temp",   3, MB_TAB_INPUT,    0, 2, 2000 },  /* 温度 0.1 ℃、湿度 0.1 %：变化慢 */
};

#define NDEV(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

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
    UART_HandleTypeDef   *huart;
    const poll_dev_cfg_t *cfgs;
    uint8_t               ndev;
    port_uart_t           port;
    mb_io_t               io;
    mb_master_t           mb;
    poll_dev_t            devs[4];
    poll_ch_t             poll;
    uint8_t               ready;
} channel_t;

static channel_t s_ch[2] = {
    { .huart = &huart2, .cfgs = k_ch1_devs, .ndev = NDEV(k_ch1_devs), .io = { .name = "ch1" } },
    { .huart = &huart4, .cfgs = k_ch2_devs, .ndev = NDEV(k_ch2_devs), .io = { .name = "ch2" } },
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

    if (c->ndev > sizeof(c->devs) / sizeof(c->devs[0])
        || port_uart_init(&c->port, c->huart, c->io.name) != 0) {
        LOGE(c->io.name, "init failed");
        vTaskDelete(NULL);
        return;
    }
    mb_master_init(&c->mb, &c->io, RSP_TIMEOUT_MS, RETRIES);
    c->mb.echo = MB_ECHO_NONE;      /* 中控的收发器不回显（实测见 docs/debug-log.md），见 mb_master.h */
    if (poll_ch_init(&c->poll, &c->mb, c->devs, c->cfgs, c->ndev) != 0) {
        LOGE(c->io.name, "bad device table");
        vTaskDelete(NULL);
        return;
    }
    c->ready = 1;
    LOGI(c->io.name, "poll start: %u device(s), timeout %u ms, retries %u",
         (unsigned)c->ndev, (unsigned)RSP_TIMEOUT_MS, (unsigned)RETRIES);

    next_report = io_now_ms() + REPORT_MS;
    for (;;) {
        poll_ch_step(&c->poll);

        if ((int32_t)(io_now_ms() - next_report) >= 0) {
            next_report += REPORT_MS;
            poll_ch_report(&c->poll);
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
    /* 栈 768 字（3 KB）。估算：mb_master_request 的回应缓冲 256 B + transact_once 里帧组装器约 270 B、
     * chunk/raw 96 B + poll_one 约 100 B + log_write 的行缓冲和 vsnprintf，合计 1.5 KB 左右，留一倍余量。
     * 上板后用 uxTaskGetStackHighWaterMark() 看实际剩多少（第 3 步笔记里的实测表） */
    BaseType_t r1 = xTaskCreate(poll_task, "ch1", POLL_STACK_WORDS, &s_ch[0], tskIDLE_PRIORITY + 3, NULL);
    BaseType_t r2 = xTaskCreate(poll_task, "ch2", POLL_STACK_WORDS, &s_ch[1], tskIDLE_PRIORITY + 3, NULL);

    configASSERT(r1 == pdPASS && r2 == pdPASS);
}

const poll_ch_t *poll_get(uint8_t ch)
{
    if (ch >= 2 || !s_ch[ch].ready)
        return NULL;
    return &s_ch[ch].poll;
}
