/*
 * file_rx.c - 文件接收状态机，说明见 file_rx.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "crc32.h"
#include "file_rx.h"

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void file_rx_init(file_rx_t *f, uint8_t *buf, uint32_t cap)
{
    memset(f, 0, sizeof(*f));
    f->buf = buf;
    f->cap = cap;
}

static mb_ex_t fail(file_rx_t *f, frx_err_t e, mb_ex_t ex)
{
    f->last_err = e;
    f->state = FRX_IDLE;            /* 作废，要从文件头重来 */
    return ex;
}

/* 第 n 包（从 1 开始）应该有多少字节：224，最后一包是剩下的，凑成偶数 */
static uint16_t expect_len(const file_rx_t *f)
{
    uint32_t left = f->hdr.length - f->received;
    uint32_t n = left < FILE_REC_DATA ? left : FILE_REC_DATA;
    return (uint16_t)((n + 1u) & ~1u);
}

mb_ex_t file_rx_record(file_rx_t *f, uint16_t record_no, const uint8_t *data, uint16_t len, int *complete)
{
    uint32_t take;

    *complete = 0;

    if (record_no == 0) {                                   /* 文件头：重新开始 */
        if (len != FILE_HDR_SIZE)
            return fail(f, FRX_ERR_HEADER, MB_EX_ILLEGAL_VALUE);
        f->hdr.version   = get32(&data[0]);
        f->hdr.length    = get32(&data[4]);
        f->hdr.load_addr = get32(&data[8]);
        f->hdr.crc32     = get32(&data[12]);
        memcpy(f->hdr.name, &data[16], 16);
        f->hdr.name[16]  = '\0';
        if (f->hdr.length == 0 || f->hdr.length > f->cap)
            return fail(f, FRX_ERR_HEADER, MB_EX_ILLEGAL_VALUE);
        f->state = FRX_RECEIVING;
        f->next_rec = 1;
        f->received = 0;
        f->last_len = 0;
        f->last_err = FRX_OK;
        return MB_EX_NONE;
    }

    /* 上位机没收到上一包的回应，又发了一次：回成功，不再写 */
    if (f->state != FRX_IDLE && record_no == (uint16_t)(f->next_rec - 1u) && len == f->last_len)
        return MB_EX_NONE;

    if (f->state != FRX_RECEIVING)
        return fail(f, FRX_ERR_NO_HEADER, MB_EX_ILLEGAL_VALUE);
    if (record_no != f->next_rec)
        return fail(f, FRX_ERR_SEQUENCE, MB_EX_ILLEGAL_VALUE);
    if (len != expect_len(f))
        return fail(f, FRX_ERR_LENGTH, MB_EX_ILLEGAL_VALUE);

    take = f->hdr.length - f->received;                     /* 最后一包可能多一个凑偶数的字节，不要它 */
    if (take > len)
        take = len;
    memcpy(&f->buf[f->received], data, take);
    f->received += take;
    f->next_rec++;
    f->last_len = len;

    if (f->received < f->hdr.length)
        return MB_EX_NONE;

    if (crc32_update(0, f->buf, f->hdr.length) != f->hdr.crc32)
        return fail(f, FRX_ERR_CRC, MB_EX_DEVICE_FAILURE);
    f->state = FRX_DONE;
    *complete = 1;
    return MB_EX_NONE;
}
