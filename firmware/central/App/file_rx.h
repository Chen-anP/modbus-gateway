/*
 * file_rx.h - 0x15（写文件记录）的文件接收状态机
 *
 * 上位机发一个文件 = 一个文件头 + 若干数据包，每包是一帧 0x15 请求（设计文档 4.3 节）：
 *
 *   record_no 0      文件头 32 字节，大端：版本(4) 长度(4) 加载地址(4) CRC32(4) 文件名(16)
 *   record_no 1、2…  数据，每包 224 字节，最后一包是剩下的（凑成偶数字节，0x15 以 2 字节为单位）
 *
 * 规则：
 *   - 收到文件头就重新开始（上一次没收完的作废）
 *   - 数据包必须按顺序：收到 N 时期望的就是 N
 *   - 重复的上一包（上位机没收到回应又重发）：回成功，数据不再写一遍
 *   - 跳包、乱序、长度不对：拒绝（异常 03），这次传输作废，要从文件头重来
 *   - 收齐后算整个文件的 CRC32，和文件头里的比较；不对回异常 04
 *
 * 为什么每包 224 字节而不是课程的 240：0x15 子请求的数据最多 238 字节（规范的字节数上限 0xF5 减 7），
 * 240 超了规范；224 是 16 和 4 的公倍数（H5 的 Flash 按 16 字节写，F030 按 4 字节）。
 *
 * 纯逻辑，PC 上测试：tests/test_file_rx.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FILE_RX_H
#define FILE_RX_H

#include <stdint.h>
#include "mb_rtu.h"

#define FILE_HDR_SIZE   32u
#define FILE_REC_DATA   224u

typedef struct {
    uint32_t version;
    uint32_t length;
    uint32_t load_addr;
    uint32_t crc32;
    char     name[17];      /* 16 字节 + 结尾 0 */
} file_hdr_t;

typedef enum {
    FRX_IDLE = 0,           /* 没有正在收的文件 */
    FRX_RECEIVING,          /* 收到文件头，正在收数据 */
    FRX_DONE                /* 收齐且 CRC 正确 */
} frx_state_t;

/* 出错原因（LAST_ERR = 0x0300 + 这个值） */
typedef enum {
    FRX_OK = 0,
    FRX_ERR_NO_HEADER,      /* 没收到文件头就来了数据 */
    FRX_ERR_HEADER,         /* 文件头长度不对、文件长度 0 或超过缓冲区 */
    FRX_ERR_SEQUENCE,       /* 跳包 / 乱序 */
    FRX_ERR_LENGTH,         /* 这一包的长度不对 */
    FRX_ERR_CRC             /* 收齐了但 CRC32 不对 */
} frx_err_t;

typedef struct {
    uint8_t    *buf;        /* 调用者提供的缓冲区 */
    uint32_t    cap;
    frx_state_t state;
    file_hdr_t  hdr;
    uint16_t    next_rec;   /* 期望的下一个 record_no */
    uint32_t    received;   /* 已经收到的字节 */
    frx_err_t   last_err;
    uint16_t    last_len;   /* 上一包的长度（判断重发用） */
} file_rx_t;

void file_rx_init(file_rx_t *f, uint8_t *buf, uint32_t cap);

/* 处理一个 0x15 子请求。返回给上位机的异常码（MB_EX_NONE = 成功）。
 * 这一包让文件收齐且 CRC 正确时，*complete = 1（f->buf 里是完整文件，长度 f->hdr.length） */
mb_ex_t file_rx_record(file_rx_t *f, uint16_t record_no, const uint8_t *data, uint16_t len, int *complete);

#endif /* FILE_RX_H */
