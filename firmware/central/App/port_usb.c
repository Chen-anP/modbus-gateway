/*
 * port_usb.c - USB 虚拟串口收发层，说明见 port_usb.h
 *
 * 为什么用 TinyUSB 而不是 USBX：CubeMX 在 H5 上只允许 USBX 和 ThreadX 一起用；
 * 手动移植 USBX 要用不依赖 RTOS 的 standalone 模式（任务里不停轮询），文件约 80 个。
 * TinyUSB 原生支持 FreeRTOS 和 STM32H5，只需要 6 个源文件，接口简单，MIT 许可。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "tusb.h"
#include "log.h"
#include "port_usb.h"

#define USBD_STACK_WORDS    512u                        /* TinyUSB 例子用 3 × 最小栈；枚举时回调里要打日志，给 2 KB */
#define USBD_PRIORITY       (tskIDLE_PRIORITY + 5)      /* 比 up_link 高：USB 事件（枚举、端点完成）要及时处理 */

static SemaphoreHandle_t s_rx_sem;      /* 收到数据时由 tud_cdc_rx_cb 给出 */
static volatile uint8_t  s_ready;       /* tusb_init 已完成，中断可以交给 TinyUSB */
static port_usb_stats_t  s_st;

/* usbd 任务：初始化 TinyUSB，然后一直处理 USB 事件。
 * TinyUSB 要求在调度器启动后初始化：它的中断处理会用 FreeRTOS 的队列 */
static void usbd_task(void *arg)
{
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    const tud_cdc_configure_t cdc_cfg = {
        .rx_persistent = false,
        .tx_persistent = false,
        .tx_overwritabe_if_not_connected = false,   /* 默认是 true：没打开串口时新数据覆盖旧数据。
                                                       我们的回应一个字节都不能丢，宁可写超时 */
    };
    (void)arg;

    tud_cdc_configure(&cdc_cfg);
    if (!tusb_init(0, &init)) {
        LOGE("usb", "tusb_init failed");
        vTaskDelete(NULL);
        return;
    }
    s_ready = 1;
    LOGI("usb", "TinyUSB started, waiting for host");

    for (;;)
        tud_task();             /* 没有事件时阻塞在 TinyUSB 的事件队列上，不占 CPU */
}

int port_usb_init(void)
{
    s_rx_sem = xSemaphoreCreateBinary();
    if (s_rx_sem == NULL)
        return -1;
    if (xTaskCreate(usbd_task, "usbd", USBD_STACK_WORDS, NULL, USBD_PRIORITY, NULL) != pdPASS)
        return -1;
    return 0;
}

int port_usb_irq(void)
{
    if (!s_ready)
        return 0;
    tud_int_handler(0);
    return 1;
}

uint16_t port_usb_read(uint8_t *buf, uint16_t max, uint32_t timeout_ms)
{
    uint32_t n;

    if (tud_cdc_available() == 0) {
        (void)xSemaphoreTake(s_rx_sem, 0);          /* 清掉旧信号：FIFO 已经空了 */
        if (tud_cdc_available() == 0 && xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
            return 0;
    }
    n = tud_cdc_read(buf, max);
    s_st.rx_bytes += n;
    return (uint16_t)n;
}

int port_usb_write(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint16_t done = 0;

    while (done < len) {
        uint32_t n = tud_cdc_write(&data[done], (uint32_t)(len - done));   /* FIFO 有多少空间写多少 */

        done = (uint16_t)(done + n);
        tud_cdc_write_flush();                      /* 启动 IN 传输：不等 FIFO 满就发出去 */
        if (done < len) {
            if (!tud_mounted() || (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
                s_st.tx_timeouts++;
                return -1;
            }
            vTaskDelay(1);                          /* FIFO 满了：等电脑取走一些 */
        }
    }
    s_st.tx_bytes += len;
    return 0;
}

void port_usb_flush(void)
{
    tud_cdc_read_flush();
}

const port_usb_stats_t *port_usb_stats(void)
{
    return &s_st;
}

/* ---------------------------------------------------------------- TinyUSB 回调（都在 usbd 任务里执行） */

void tud_mount_cb(void)
{
    s_st.mounted = 1;
    s_st.mounts++;
    LOGI("usb", "mounted (enumerated by host)");
}

void tud_umount_cb(void)
{
    s_st.mounted = 0;
    LOGI("usb", "unmounted");
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf; (void)rts;
    s_st.dtr = dtr ? 1u : 0u;
    LOGI("usb", "port %s on PC (DTR %u)", dtr ? "opened" : "closed", (unsigned)dtr);
}

void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    xSemaphoreGive(s_rx_sem);       /* 任务上下文，用普通版本 */
}
