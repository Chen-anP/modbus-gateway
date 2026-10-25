/*
 * mb_master.h - Modbus RTU 主机请求层：一次"发请求 → 收回应 → 校验"，带重试和统计
 *
 * 在 mb_rtu（纯协议）和串口之间：
 *   mb_rtu     组帧、切帧、解析，不知道字节从哪来
 *   mb_master  把请求发出去、把回应收回来、处理回显和超时、失败重试、计数
 *   mb_io_t    一组函数指针，真正的收发由调用者提供（板子上是 port_uart，PC 测试里是模拟从机）
 *
 * 不依赖 FreeRTOS、HAL，可以在 PC 上测试（tests/test_poll.c）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef MB_MASTER_H
#define MB_MASTER_H

#include <stdint.h>
#include "mb_rtu.h"

/* 收发接口：每个通道一份 */
typedef struct {
    void       *ctx;                                                    /* 原样传回，板子上是 port_uart_t * */
    const char *name;                                                   /* 日志用，如 "ch1" */
    int       (*send)(void *ctx, const uint8_t *data, uint16_t len, uint32_t timeout_ms);  /* 成功 0 */
    uint16_t  (*read)(void *ctx, uint8_t *buf, uint16_t max, uint32_t timeout_ms);       /* 0 = 超时 */
    void      (*flush)(void *ctx);                                      /* 丢弃没读走的数据 */
    uint32_t  (*now_ms)(void);                                          /* 当前时间，毫秒 */
    void      (*delay_ms)(uint32_t ms);                                 /* 让出 CPU 等一会儿 */
} mb_io_t;

/* 统计：只增不减。"尝试"指每一次实际发出的请求（含重试） */
typedef struct {
    uint32_t requests;      /* 调用 mb_master_request 的次数 */
    uint32_t ok;            /* 成功（含回异常码的）次数 */
    uint32_t exceptions;    /* 从机回异常码 */
    uint32_t timeouts;      /* 尝试中：超时没收到完整回应 */
    uint32_t crc_errors;    /* 尝试中：收到了但 CRC 错 */
    uint32_t frame_errors;  /* 尝试中：地址 / 功能码 / 长度对不上 */
    uint32_t send_errors;   /* 尝试中：发送失败 */
    uint32_t retries;       /* 重试次数（第 2、3……次尝试） */
} mb_master_stats_t;

/* 收发器会不会把自己发的字节收回来（回显）
 *   AUTO   不知道：收到的字节和请求逐字节一样就当回显跳过（第 2 步 bus_probe 的做法）。
 *          问题：写单个线圈 / 寄存器（05 / 06）的正常回应和请求一模一样，会被当成回显吃掉。
 *          所以 AUTO 下 05 / 06 只收到一份时，等到超时再把它当回应——写一次要等满 rsp_timeout_ms。
 *   NONE   收发器不回显：不做回显检查（最快，也没有歧义）。中控板实测属于这种（docs/debug-log.md：
 *          传感器不回应时日志是 "no byte received"，连自己发的字节都没收到）
 *   ALWAYS 一定回显：每次都先跳过一份请求，05 / 06 要收到两份才算成功 */
typedef enum {
    MB_ECHO_AUTO = 0,
    MB_ECHO_NONE,
    MB_ECHO_ALWAYS
} mb_echo_t;

typedef struct {
    const mb_io_t    *io;
    mb_echo_t         echo;             /* 默认 AUTO，init 之后可以改 */
    uint32_t          rsp_timeout_ms;   /* 发完请求后，等回应最多多久 */
    uint32_t          byte_gap_ms;      /* 回应中两个字节间隔超过它，丢弃半帧 */
    uint32_t          turnaround_ms;    /* 收到回应后，至少隔这么久再发下一个请求（帧间隔） */
    uint8_t           retries;          /* 失败后再试几次 */
    uint32_t          last_rx_ms;       /* 上一次收到回应的时间 */
    mb_master_stats_t st;
} mb_master_t;

void mb_master_init(mb_master_t *m, const mb_io_t *io, uint32_t rsp_timeout_ms, uint8_t retries);

/* 发送一帧已经组好的请求（含 CRC），等回应并解析。
 * 读请求的结果写到 values（位数据每个点一个元素，值 0/1）。
 * 返回 MB_OK；MB_ERR_EXCEPTION（异常码在 *ex，不重试：从机是活的，重试也一样）；
 *      MB_ERR_TIMEOUT / MB_ERR_CRC / MB_ERR_FRAME（已经重试过 retries 次）。 */
int mb_master_request(mb_master_t *m, const uint8_t *req, uint16_t req_len,
                      uint16_t *values, uint16_t max_values, mb_ex_t *ex);

/* 读：组帧 + mb_master_request。参数非法返回 MB_ERR_PARAM */
int mb_master_read(mb_master_t *m, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t count,
                   uint16_t *values, uint16_t max_values, mb_ex_t *ex);

/* 写单个线圈 / 保持寄存器 */
int mb_master_write_single(mb_master_t *m, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t value,
                           mb_ex_t *ex);

#endif /* MB_MASTER_H */
