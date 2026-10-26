/*
 * table_loader.h - 上位机通过 0x15 下发点表：收文件 → 校验 CRC → 解析到临时表
 *
 *   file_no 0x0000 = 点表（设计文档 4.3 节）；其他 file_no（固件）第 7、8 步再支持，现在回异常 02
 *
 * 只负责"收到一张合法的新表"，不负责换上：换表要等两个通道任务都停在一轮之间，
 * 由 gw_data.c 在回应发出去以后做（gw_apply_table）。
 *
 * 出错时 last_err（写进系统区 LAST_ERR；收到一张合法的新表时 LAST_ERR 清零）：
 *   0x01xx  点表内容不合法（低字节 = pt_err_t），整张拒绝，旧表继续用
 *   0x03xx  文件传输出错（低字节 = frx_err_t）：没有文件头、跳包、长度不对、CRC 不对
 *
 * 纯逻辑，PC 上测试：tests/test_table_loader.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef TABLE_LOADER_H
#define TABLE_LOADER_H

#include <stdint.h>
#include "mb_rtu.h"
#include "file_rx.h"
#include "point_table.h"

#define FILE_NO_POINT_TABLE 0x0000u

typedef struct {
    file_rx_t     rx;
    uint8_t       buf[PT_MAX_POINTS * PT_REC_SIZE];
    point_table_t tmp;          /* 解析成功的新表，等着被换上 */
    uint8_t       ready;        /* tmp 里有一张合法的、还没换上的表 */
    uint16_t      last_err;     /* 最近一次出错的原因，0 = 没出错 */
    uint16_t      bad_point;    /* 点表错误时，第几个点 */
    uint32_t      tables_ok;    /* 统计：收到几张合法的表 */
} table_loader_t;

void table_loader_init(table_loader_t *t);

/* 0x15 回调里调用。返回给上位机的异常码 */
mb_ex_t table_loader_record(table_loader_t *t, uint16_t file_no, uint16_t record_no,
                            const uint8_t *data, uint16_t len);

#endif /* TABLE_LOADER_H */
