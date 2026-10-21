/*
 * bus_probe.c - 第 2 步的测试任务：每秒向两路 RS485 上的传感器各发一次"读输入寄存器"请求，
 *               打印收发的原始字节和解析出来的数值。
 *
 * 第 3 步会把这里的"发请求、收回应"整理成正式的轮询模块（point_sync），这个文件届时删除。
 *
 * 一次请求-回应的过程（transact 函数）：
 *   1. 清空接收缓冲区里的残留数据
 *   2. 组帧：mb_build_read() → 01 04 00 00 00 02 + CRC
 *   3. DMA 发送，等发完
 *   4. 循环读串口，把字节喂给帧组装器 mb_framer_feed()，凑够一整帧就停
 *      （帧有多长由功能码和长度字段算出来，见 mb_rtu.c 的 mb_frame_need）
 *   5. 超时还没凑够 → 失败
 *   6. mb_parse_rsp() 校验 CRC、地址、功能码、长度，取出寄存器值
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "usart.h"
#include "log.h"
#include "mb_rtu.h"
#include "port_uart.h"
#include "bus_probe.h"

#define RSP_TIMEOUT_MS  100u    /* 等回应最多 100 ms：传感器一般几毫秒内就回 */
#define BYTE_GAP_MS     5u      /* 两个字节之间超过 5 ms 没来，丢弃半帧 */

static port_uart_t s_ch1;       /* USART2：开关量（地址 1）、环境监测（地址 2） */
static port_uart_t s_ch2;       /* UART4：温湿度（地址 3） */

/* 把一段字节喂给帧组装器；凑成一整帧时返回 1，并把帧复制到 rsp */
static int feed(mb_framer_t *f, const uint8_t *d, uint16_t n, uint8_t *rsp, uint16_t *rsp_len)
{
    uint16_t used;

    while (n > 0) {
        if (mb_framer_feed(f, d, n, (uint32_t)xTaskGetTickCount(), &used)) {
            memcpy(rsp, f->buf, f->len);
            *rsp_len = f->len;
            return 1;
        }
        d += used;
        n = (uint16_t)(n - used);
    }
    return 0;
}

/* 一次请求-回应。返回回应长度（>0），0 = 超时（会打印收到了什么），-1 = 发送失败 */
static int transact(port_uart_t *p, const uint8_t *req, uint16_t req_len, uint8_t *rsp)
{
    mb_framer_t f;
    uint8_t chunk[64];
    uint16_t n, rsp_len = 0;
    uint16_t echo_pos = 0;          /* 已经和请求逐字节对上的个数 */
    int echo_check = 1;             /* 还在检查"是不是回显" */
    uint8_t raw[32];                /* 诊断用：记下这次收到的前 32 个原始字节，超时时打印 */
    uint16_t raw_len = 0;
    TickType_t start;

    mb_framer_init(&f, 0, BYTE_GAP_MS);         /* 0 = 按"回应帧"格式判断长度 */
    port_uart_flush(p);

    log_hex(LOG_LVL_DEBUG, p->name, "tx", req, req_len);
    if (port_uart_send(p, req, req_len, 50) != 0)
        return -1;

    start = xTaskGetTickCount();
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        const uint8_t *d = chunk;

        if (elapsed >= pdMS_TO_TICKS(RSP_TIMEOUT_MS))
            break;                              /* 超时 */
        n = port_uart_read(p, chunk, sizeof(chunk), RSP_TIMEOUT_MS - elapsed * portTICK_PERIOD_MS);
        if (n == 0)
            break;
        for (uint16_t k = 0; k < n && raw_len < sizeof(raw); k++)
            raw[raw_len++] = chunk[k];

        /* 有的 RS485 模块会把自己发出去的数据也收回来（回显）。
         * 收到的字节和请求逐个比较：整帧都对上 → 是回显，丢掉；
         * 中途对不上 → 不是回显（正常回应的开头本来就和请求一样：地址、功能码），
         *              把先前跳过的那几个字节补喂给帧组装器 */
        while (echo_check && n > 0) {
            if (*d == req[echo_pos]) {
                d++; n--;
                if (++echo_pos == req_len) {
                    echo_check = 0;
                    LOGD(p->name, "echo of %u bytes skipped", (unsigned)req_len);
                }
            } else {
                echo_check = 0;
                if (feed(&f, req, echo_pos, rsp, &rsp_len))
                    break;
            }
        }
        if (rsp_len == 0 && feed(&f, d, n, rsp, &rsp_len) == 0)
            continue;                           /* 还没凑够一帧，继续读 */

        log_hex(LOG_LVL_DEBUG, p->name, "rx", rsp, rsp_len);
        return rsp_len;
    }

    /* 超时：打印收到了什么，帮助判断问题在哪一侧
     *   一个字节都没有 → 对方没回：没烧程序、地址不对、接线（A/B 反、没共地）、没上电
     *   只有回显       → 我们发出去了，对方没回
     *   有字节但不成帧 → 收到了乱码：波特率不对、A/B 接反、干扰 */
    if (raw_len == 0)
        LOGW(p->name, "no byte received in %u ms", (unsigned)RSP_TIMEOUT_MS);
    else if (!echo_check && echo_pos == req_len && raw_len == req_len)
        LOGW(p->name, "only echo received, no reply");
    else
        log_hex(LOG_LVL_WARN, p->name, "incomplete", raw, raw_len);
    return 0;
}

/* 读一个传感器的输入寄存器并打印 */
static void probe(port_uart_t *p, uint8_t slave, uint16_t addr, uint16_t count)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t vals[8];
    uint16_t req_len = mb_build_read(req, slave, MB_TAB_INPUT, addr, count);
    int rsp_len = transact(p, req, req_len, rsp);
    mb_ex_t ex;
    int rc;

    if (rsp_len < 0) {
        LOGE(p->name, "dev %u: send failed", slave);
        return;
    }
    if (rsp_len == 0) {
        LOGW(p->name, "dev %u: timeout", slave);
        return;
    }
    rc = mb_parse_rsp(req, req_len, rsp, (uint16_t)rsp_len, vals, 8, &ex);
    if (rc == MB_OK)
        LOGI(p->name, "dev %u: 3x[%u..%u] = %u, %u", slave, addr, addr + count - 1, vals[0], vals[1]);
    else if (rc == MB_ERR_EXCEPTION)
        LOGW(p->name, "dev %u: exception %u", slave, (unsigned)ex);
    else
        LOGW(p->name, "dev %u: bad response (err %d)", slave, rc);
}

static void bus_probe_task(void *arg)
{
    uint32_t round = 0;
    (void)arg;

    if (port_uart_init(&s_ch1, &huart2, "ch1") != 0 || port_uart_init(&s_ch2, &huart4, "ch2") != 0) {
        LOGE("probe", "port init failed");
        vTaskDelete(NULL);
    }
    LOGI("probe", "start: ch1=USART2, ch2=UART4, 115200 8N1");

    for (;;) {
        probe(&s_ch1, 1, 0, 2);     /* 开关量：没有输入寄存器，回 exception 2 就算通 */
        probe(&s_ch1, 2, 0, 2);     /* 环境监测：两路 ADC */
        probe(&s_ch1, 3, 0, 2);     /* 温湿度 */
        probe(&s_ch2, 1, 0, 2);
        probe(&s_ch2, 2, 0, 2);
        probe(&s_ch2, 3, 0, 2);

        if (++round % 10 == 0) {    /* 每 10 秒打印一次统计 */
            LOGI("probe", "ch1 tx %lu rx %lu ev %lu err %lu ovf %lu | ch2 tx %lu rx %lu ev %lu err %lu ovf %lu",
                 (unsigned long)s_ch1.tx_bytes, (unsigned long)s_ch1.rx_bytes, (unsigned long)s_ch1.rx_events,
                 (unsigned long)s_ch1.rx_errors, (unsigned long)s_ch1.rx_overflow,
                 (unsigned long)s_ch2.tx_bytes, (unsigned long)s_ch2.rx_bytes, (unsigned long)s_ch2.rx_events,
                 (unsigned long)s_ch2.rx_errors, (unsigned long)s_ch2.rx_overflow);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void bus_probe_start(void)
{
    xTaskCreate(bus_probe_task, "probe", 512, NULL, tskIDLE_PRIORITY + 3, NULL);   /* 栈 2 KB：两个 256 字节的帧缓冲在栈上 */
}
