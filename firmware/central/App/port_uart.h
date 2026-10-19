/*
 * port_uart.h - 串口收发层：DMA + 空闲中断接收，DMA 发送
 *
 * 用法（每个串口一个对象，由一个任务独占使用）：
 *   static port_uart_t s_ch2;
 *   port_uart_init(&s_ch2, &huart4, "ch2");
 *   port_uart_send(&s_ch2, req, len, 50);          // 发送，等发完（最后一个字节移出移位寄存器）
 *   n = port_uart_read(&s_ch2, buf, sizeof(buf), 100);  // 读，最多等 100 ms
 *
 * 接收的数据流：
 *   串口 → DMA 自动搬进 dma_buf → 空闲 / 半满 / 全满事件（中断）
 *        → HAL_UARTEx_RxEventCallback 把新收到的字节放进流缓冲区（StreamBuffer）
 *        → 任务调用 port_uart_read 从流缓冲区取（没有数据时阻塞，不占 CPU）
 *
 * 约束：
 *   1. 每个端口只能由一个任务读、一个任务写（流缓冲区的规则：一个写者、一个读者）
 *   2. port_uart_send 期间调用者阻塞，所以发送缓冲区可以是调用者的局部变量
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef PORT_UART_H
#define PORT_UART_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "semphr.h"
#include "main.h"

#define PORT_UART_DMA_BUF   256u    /* DMA 接收缓冲区：一帧 Modbus 最长 256 字节 */
#define PORT_UART_SB_SIZE   512u    /* 流缓冲区：任务来不及取时最多攒 512 字节 */

typedef struct {
    UART_HandleTypeDef  *huart;
    const char          *name;
    StreamBufferHandle_t rx_sb;     /* 中断 → 任务 的数据管道 */
    SemaphoreHandle_t    tx_done;   /* 发送完成时由中断"给"出，发送函数在这里等 */
    uint8_t              dma_buf[PORT_UART_DMA_BUF];
    uint16_t             dma_pos;   /* 本轮 DMA 接收里，已经交给流缓冲区的字节数 */

    /* 统计（只增不减，调试用） */
    volatile uint32_t rx_bytes;     /* 成功放进流缓冲区的字节 */
    volatile uint32_t rx_overflow;  /* 流缓冲区满被丢掉的字节 */
    volatile uint32_t rx_events;    /* 接收事件次数（空闲 / 半满 / 全满） */
    volatile uint32_t rx_errors;    /* 帧错误、噪声、溢出等硬件错误次数 */
    volatile uint32_t tx_bytes;
    volatile uint32_t tx_timeouts;
} port_uart_t;

/* 初始化并开始接收。在任务里调用（不要在中断里）。成功返回 0。 */
int port_uart_init(port_uart_t *p, UART_HandleTypeDef *huart, const char *name);

/* DMA 发送，阻塞等待发送完成或超时。成功返回 0，失败 / 超时返回 -1。 */
int port_uart_send(port_uart_t *p, const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* 读取已收到的数据：有数据立刻返回（最多 max 字节）；没有数据最多等 timeout_ms。
 * 返回读到的字节数，0 表示超时。 */
uint16_t port_uart_read(port_uart_t *p, uint8_t *buf, uint16_t max, uint32_t timeout_ms);

/* 丢弃还没读走的数据（发请求前调用，避免把上一次的残留当成回应） */
void port_uart_flush(port_uart_t *p);

#endif /* PORT_UART_H */
