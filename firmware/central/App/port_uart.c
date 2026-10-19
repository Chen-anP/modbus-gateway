/*
 * port_uart.c - 串口收发层，说明见 port_uart.h
 *
 * HAL 的三个回调函数（接收事件、发送完成、错误）是全局的、所有串口共用一个，
 * 所以这里维护一张"huart → port_uart_t"的登记表，回调里先查表找到是哪个端口。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "port_uart.h"     /* 里面先包含了 FreeRTOS.h：FreeRTOS 要求它排在 task.h 之前 */
#include "task.h"

#define MAX_PORTS 4u

static port_uart_t *s_ports[MAX_PORTS];

static port_uart_t *find_port(UART_HandleTypeDef *huart)
{
    for (uint32_t i = 0; i < MAX_PORTS; i++)
        if (s_ports[i] != NULL && s_ports[i]->huart == huart)
            return s_ports[i];
    return NULL;
}

/* 启动（或重新启动）一轮 DMA 接收：收满 256 字节、或者线路空闲时，产生一次接收事件 */
static void start_rx(port_uart_t *p)
{
    p->dma_pos = 0;
    if (HAL_UARTEx_ReceiveToIdle_DMA(p->huart, p->dma_buf, PORT_UART_DMA_BUF) != HAL_OK)
        p->rx_errors++;
}

int port_uart_init(port_uart_t *p, UART_HandleTypeDef *huart, const char *name)
{
    uint32_t i;

    memset(p, 0, sizeof(*p));
    p->huart   = huart;
    p->name    = name;
    p->rx_sb   = xStreamBufferCreate(PORT_UART_SB_SIZE, 1);   /* 触发等级 1：收到 1 个字节就唤醒读者 */
    p->tx_done = xSemaphoreCreateBinary();
    if (p->rx_sb == NULL || p->tx_done == NULL)
        return -1;

    taskENTER_CRITICAL();                   /* 登记表也会被中断读，修改时关中断 */
    for (i = 0; i < MAX_PORTS && s_ports[i] != NULL; i++) { }
    if (i < MAX_PORTS)
        s_ports[i] = p;
    taskEXIT_CRITICAL();
    if (i >= MAX_PORTS)
        return -1;

    start_rx(p);
    return 0;
}

int port_uart_send(port_uart_t *p, const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    (void)xSemaphoreTake(p->tx_done, 0);    /* 清掉可能残留的"完成"信号 */

    if (HAL_UART_Transmit_DMA(p->huart, data, len) != HAL_OK)
        return -1;

    /* 等发送完成中断。DMA 发送时 CPU 是空闲的，这个任务阻塞，别的任务可以运行 */
    if (xSemaphoreTake(p->tx_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)HAL_UART_AbortTransmit(p->huart);
        p->tx_timeouts++;
        return -1;
    }
    p->tx_bytes += len;
    return 0;
}

uint16_t port_uart_read(port_uart_t *p, uint8_t *buf, uint16_t max, uint32_t timeout_ms)
{
    return (uint16_t)xStreamBufferReceive(p->rx_sb, buf, max, pdMS_TO_TICKS(timeout_ms));
}

void port_uart_flush(port_uart_t *p)
{
    (void)xStreamBufferReset(p->rx_sb);     /* 只有本端口的读者（就是调用者自己）会阻塞在它上面，所以能重置 */
}

/* ================================================================ 中断回调（HAL 调用） */

/*
 * 接收事件。三种情况会进来：
 *   HT   半满：DMA 收了 128 字节（数据还在继续来）
 *   TC   全满：DMA 收满 256 字节，这一轮结束
 *   IDLE 空闲：线路空闲了约 1 个字节的时间，说明对方这一段发完了，这一轮结束
 * size 是本轮 DMA 已经收到的总字节数，dma_pos 是之前已经交出去的部分，
 * 所以这次新来的数据是 dma_buf[dma_pos .. size)。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    BaseType_t woken = pdFALSE;
    port_uart_t *p = find_port(huart);

    if (p == NULL)
        return;
    p->rx_events++;

    if (size > p->dma_pos) {
        size_t n    = (size_t)(size - p->dma_pos);
        size_t sent = xStreamBufferSendFromISR(p->rx_sb, &p->dma_buf[p->dma_pos], n, &woken);
        p->rx_bytes += (uint32_t)sent;
        if (sent < n)
            p->rx_overflow += (uint32_t)(n - sent);    /* 任务来不及取，流缓冲区满了 */
    }

    if (HAL_UARTEx_GetRxEventType(huart) == HAL_UART_RXEVENT_HT)
        p->dma_pos = size;          /* 半满：这一轮还没结束，记住交到哪里了 */
    else
        start_rx(p);                /* 全满或空闲：HAL 已经停止了这一轮，马上开始下一轮 */

    portYIELD_FROM_ISR(woken);      /* 如果唤醒了更高优先级的任务，中断返回后立刻切换过去 */
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t woken = pdFALSE;
    port_uart_t *p = find_port(huart);

    if (p == NULL)
        return;
    (void)xSemaphoreGiveFromISR(p->tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* 帧错误、噪声、溢出等：HAL 会停止接收，计数后重新开始 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    port_uart_t *p = find_port(huart);

    if (p == NULL)
        return;
    p->rx_errors++;
    (void)HAL_UART_AbortReceive(huart);
    start_rx(p);
}
