/*
 * test_file_rx.c - CRC32 和文件接收状态机：正常、分包、重发、跳包、乱序、长度、CRC、没有文件头
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/crc32.c
 *       firmware/central/App/file_rx.c tests/test_file_rx.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "crc32.h"
#include "file_rx.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void make_hdr(uint8_t *h, uint32_t len, uint32_t crc)
{
    memset(h, 0, FILE_HDR_SIZE);
    put32(&h[0], 1);
    put32(&h[4], len);
    put32(&h[8], 0);
    put32(&h[12], crc);
    memcpy(&h[16], "points.bin", 10);
}

static uint8_t g_file[600], g_buf[1024];
static file_rx_t f;

/* 按规则把文件发完：返回最后一包的异常码，*done = 有没有收齐 */
static mb_ex_t send_all(uint32_t len, int *done)
{
    uint8_t hdr[FILE_HDR_SIZE];
    uint32_t off = 0;
    uint16_t rec = 1;
    mb_ex_t ex;

    make_hdr(hdr, len, crc32_update(0, g_file, len));
    ex = file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, done);
    if (ex) return ex;
    while (off < len) {
        uint16_t n = (uint16_t)((len - off) < FILE_REC_DATA ? (len - off) : FILE_REC_DATA);
        uint16_t padded = (uint16_t)((n + 1u) & ~1u);
        uint8_t pkt[FILE_REC_DATA];
        memset(pkt, 0, sizeof(pkt));
        memcpy(pkt, &g_file[off], n);
        ex = file_rx_record(&f, rec++, pkt, padded, done);
        if (ex) return ex;
        off += n;
    }
    return MB_EX_NONE;
}

int main(void)
{
    uint8_t hdr[FILE_HDR_SIZE], pkt[FILE_REC_DATA];
    int done;

    /* 1. CRC32 标准校验值，分段算结果相同 */
    CHECK(crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(crc32_update(crc32_update(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xCBF43926u);
    CHECK(crc32_update(0, NULL, 0) == 0);

    for (unsigned i = 0; i < sizeof(g_file); i++) g_file[i] = (uint8_t)(i * 7 + 3);
    file_rx_init(&f, g_buf, sizeof(g_buf));

    /* 2. 正常：176 字节（22 个点）一包就完；收齐后 done、内容一样、文件头解析正确 */
    CHECK(send_all(176, &done) == MB_EX_NONE && done == 1);
    CHECK(f.state == FRX_DONE && memcmp(g_buf, g_file, 176) == 0);
    CHECK(f.hdr.version == 1 && f.hdr.length == 176 && strcmp(f.hdr.name, "points.bin") == 0);

    /* 3. 多包：500 字节 = 224 + 224 + 52 */
    CHECK(send_all(500, &done) == MB_EX_NONE && done == 1 && memcmp(g_buf, g_file, 500) == 0);

    /* 4. 奇数长度：最后一包凑成偶数，多出来的字节不算进文件、不影响 CRC */
    CHECK(send_all(225, &done) == MB_EX_NONE && done == 1 && f.received == 225);

    /* 5. 最后一包重发（上位机没收到回应）：回成功，不重复写、不重复报完成 */
    send_all(176, &done);
    memcpy(pkt, g_file, 176);
    CHECK(file_rx_record(&f, 1, pkt, 176, &done) == MB_EX_NONE && done == 0 && f.received == 176);

    /* 6. 中间一包重发：回成功，接着发下一包照常 */
    make_hdr(hdr, 500, crc32_update(0, g_file, 500));
    file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done);
    file_rx_record(&f, 1, g_file, 224, &done);
    CHECK(file_rx_record(&f, 1, g_file, 224, &done) == MB_EX_NONE && f.received == 224);
    file_rx_record(&f, 2, &g_file[224], 224, &done);
    CHECK(file_rx_record(&f, 3, &g_file[448], 52, &done) == MB_EX_NONE && done == 1);

    /* 7. 跳包：发了 1 直接发 3 → 异常 03，作废；之后再发 2 也不行（要从文件头重来） */
    file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done);
    file_rx_record(&f, 1, g_file, 224, &done);
    CHECK(file_rx_record(&f, 3, &g_file[448], 52, &done) == MB_EX_ILLEGAL_VALUE && f.last_err == FRX_ERR_SEQUENCE);
    CHECK(file_rx_record(&f, 2, &g_file[224], 224, &done) == MB_EX_ILLEGAL_VALUE && f.last_err == FRX_ERR_NO_HEADER);

    /* 8. 没有文件头就来数据 */
    file_rx_init(&f, g_buf, sizeof(g_buf));
    CHECK(file_rx_record(&f, 1, g_file, 224, &done) == MB_EX_ILLEGAL_VALUE && f.last_err == FRX_ERR_NO_HEADER);

    /* 9. 长度不对：中间一包不是 224 */
    file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done);
    CHECK(file_rx_record(&f, 1, g_file, 200, &done) == MB_EX_ILLEGAL_VALUE && f.last_err == FRX_ERR_LENGTH);

    /* 10. CRC 不对：最后一包回异常 04，不报完成 */
    make_hdr(hdr, 176, 0x12345678u);
    file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done);
    CHECK(file_rx_record(&f, 1, g_file, 176, &done) == MB_EX_DEVICE_FAILURE && done == 0 && f.last_err == FRX_ERR_CRC);

    /* 11. 文件头：长度 0、超过缓冲区、文件头本身不是 32 字节 */
    make_hdr(hdr, 0, 0);
    CHECK(file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done) == MB_EX_ILLEGAL_VALUE && f.last_err == FRX_ERR_HEADER);
    make_hdr(hdr, sizeof(g_buf) + 1, 0);
    CHECK(file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done) == MB_EX_ILLEGAL_VALUE);
    CHECK(file_rx_record(&f, 0, hdr, 30, &done) == MB_EX_ILLEGAL_VALUE);

    /* 12. 收到一半又来一个新文件头：重新开始 */
    make_hdr(hdr, 500, crc32_update(0, g_file, 500));
    file_rx_record(&f, 0, hdr, FILE_HDR_SIZE, &done);
    file_rx_record(&f, 1, g_file, 224, &done);
    CHECK(send_all(176, &done) == MB_EX_NONE && done == 1);

    printf("file_rx: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
