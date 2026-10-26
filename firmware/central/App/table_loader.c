/*
 * table_loader.c - 点表下发，说明见 table_loader.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "table_loader.h"

#define ERR_TABLE   0x0100u
#define ERR_FILE    0x0300u

void table_loader_init(table_loader_t *t)
{
    memset(t, 0, sizeof(*t));
    file_rx_init(&t->rx, t->buf, sizeof(t->buf));
}

mb_ex_t table_loader_record(table_loader_t *t, uint16_t file_no, uint16_t record_no,
                            const uint8_t *data, uint16_t len)
{
    int complete = 0;
    mb_ex_t ex;
    pt_err_t e;

    if (file_no != FILE_NO_POINT_TABLE)
        return MB_EX_ILLEGAL_ADDRESS;               /* 固件文件以后再支持 */

    ex = file_rx_record(&t->rx, record_no, data, len, &complete);
    if (ex != MB_EX_NONE) {
        t->last_err = (uint16_t)(ERR_FILE | (unsigned)t->rx.last_err);
        return ex;
    }
    if (!complete)
        return MB_EX_NONE;

    /* 文件收齐、CRC 也对了：解析到临时表。不合法就整张拒绝，正在用的表不受影响 */
    e = point_table_parse(&t->tmp, t->buf, t->rx.hdr.length, &t->bad_point);
    if (e != PT_OK) {
        t->ready = 0;
        t->last_err = (uint16_t)(ERR_TABLE | (unsigned)e);
        return MB_EX_DEVICE_FAILURE;
    }
    t->ready = 1;
    t->last_err = 0;
    t->tables_ok++;
    return MB_EX_NONE;
}
