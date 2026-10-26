/*
 * test_point_table.c - 点表：解析、每一条校验规则、排序、合并读块、默认表
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/point_table.c
 *       firmware/central/App/default_points.c tests/test_point_table.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "point_table.h"
#include "default_points.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static point_table_t t;
static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE + 16];

#define P(tab, ch, dev, gw, da) { tab, ch, dev, gw, da }

static pt_err_t parse(const pt_point_t *p, uint16_t n, uint16_t *bad)
{
    uint32_t len = point_table_encode(p, n, buf, sizeof(buf));
    return point_table_parse(&t, buf, len, bad);
}

int main(void)
{
    uint16_t bad;

    /* 1. 文件格式：8 字节一个点，大端 */
    {
        const pt_point_t p[] = { P(MB_TAB_INPUT, 2, 3, 0x0114, 0x0201) };
        CHECK(point_table_encode(p, 1, buf, sizeof(buf)) == 8);
        CHECK(buf[0] == 2 && buf[1] == 2 && buf[2] == 3 && buf[3] == 0);
        CHECK(buf[4] == 0x01 && buf[5] == 0x14 && buf[6] == 0x02 && buf[7] == 0x01);
        CHECK(point_table_encode(p, 1, buf, 7) == 0);           /* 空间不够 */
    }

    /* 2. 长度：0、不是 8 的倍数、点太多 */
    CHECK(point_table_parse(&t, buf, 0, &bad) == PT_ERR_LEN);
    CHECK(point_table_parse(&t, buf, 12, &bad) == PT_ERR_LEN);
    CHECK(point_table_parse(&t, buf, (PT_MAX_POINTS + 1) * PT_REC_SIZE, &bad) == PT_ERR_COUNT);

    /* 3. 每条规则，并且 bad 指出是第几个点 */
    {
        pt_point_t p[] = { P(0, 1, 1, 0, 0), P(0, 1, 1, 1, 1) };
        p[1].tab = 4;   CHECK(parse(p, 2, &bad) == PT_ERR_TAB && bad == 1);   p[1].tab = 0;
        p[1].ch = 0;    CHECK(parse(p, 2, &bad) == PT_ERR_CH && bad == 1);
        p[1].ch = 3;    CHECK(parse(p, 2, &bad) == PT_ERR_CH);                 p[1].ch = 1;
        p[1].dev = 0;   CHECK(parse(p, 2, &bad) == PT_ERR_DEV);
        p[1].dev = 248; CHECK(parse(p, 2, &bad) == PT_ERR_DEV);                p[1].dev = 1;
        p[1].gw_addr = 500; CHECK(parse(p, 2, &bad) == PT_ERR_ADDR);
        p[1].gw_addr = 499; CHECK(parse(p, 2, &bad) == PT_OK);                 /* 边界 499 可以 */
        p[1].gw_addr = 0;   CHECK(parse(p, 2, &bad) == PT_ERR_DUP && bad == 1); /* 和第 0 个同表同地址 */
        p[1].tab = 1;       CHECK(parse(p, 2, &bad) == PT_OK);                 /* 不同表的同一地址可以 */
    }

    /* 4. 排序 + 合并：乱序给出，按 <通道, 设备, 表, 传感器地址> 排好；连续的合并，有空洞就分开 */
    {
        const pt_point_t p[] = {
            P(MB_TAB_INPUT, 2, 3, 21, 1), P(MB_TAB_COIL, 1, 1, 2, 2), P(MB_TAB_COIL, 1, 1, 0, 0),
            P(MB_TAB_INPUT, 2, 3, 20, 0), P(MB_TAB_COIL, 1, 1, 1, 1), P(MB_TAB_COIL, 1, 1, 9, 5),
            P(MB_TAB_DISCRETE, 1, 1, 0, 0),
        };
        CHECK(parse(p, 7, &bad) == PT_OK && t.n == 7);
        CHECK(t.pts[0].ch == 1 && t.pts[0].tab == MB_TAB_COIL && t.pts[0].dev_addr == 0);
        CHECK(t.pts[6].ch == 2 && t.pts[6].dev_addr == 1);
        CHECK(t.nblocks == 4);
        /* 线圈 0～2 一块；线圈 5 单独（3、4 没映射）；1x 0 一块；ch2 的 3x 0～1 一块 */
        CHECK(t.blocks[0].tab == MB_TAB_COIL && t.blocks[0].start == 0 && t.blocks[0].count == 3 && t.blocks[0].first == 0);
        CHECK(t.blocks[1].start == 5 && t.blocks[1].count == 1 && t.pts[t.blocks[1].first].gw_addr == 9);
        CHECK(t.blocks[2].tab == MB_TAB_DISCRETE && t.blocks[2].count == 1);
        CHECK(t.blocks[3].ch == 2 && t.blocks[3].dev == 3 && t.blocks[3].count == 2);
        /* 按中控地址找 */
        CHECK(point_table_find(&t, MB_TAB_COIL, 9) == t.blocks[1].first);
        CHECK(point_table_find(&t, MB_TAB_COIL, 3) == -1);
    }

    /* 5. 块大小上限：20 个连续的点 → 16 + 4 */
    {
        pt_point_t p[20];
        for (uint16_t i = 0; i < 20; i++) { pt_point_t q = P(MB_TAB_HOLDING, 1, 5, i, (uint16_t)(100 + i)); p[i] = q; }
        CHECK(parse(p, 20, &bad) == PT_OK && t.nblocks == 2);
        CHECK(t.blocks[0].count == PT_BLOCK_MAX && t.blocks[1].start == 100 + PT_BLOCK_MAX && t.blocks[1].count == 4);
    }

    /* 6. 不同设备、地址恰好连续，也不能合并 */
    {
        const pt_point_t p[] = { P(MB_TAB_COIL, 1, 1, 0, 0), P(MB_TAB_COIL, 1, 2, 1, 1) };
        CHECK(parse(p, 2, &bad) == PT_OK && t.nblocks == 2);
    }

    /* 7. 内置默认表：22 个点，6 个读块（ch1 四块、ch2 两块） */
    {
        uint32_t len = default_points(buf, sizeof(buf));
        uint16_t ch1 = 0;
        CHECK(len == 22 * PT_REC_SIZE);
        CHECK(point_table_parse(&t, buf, len, &bad) == PT_OK && t.n == 22 && t.nblocks == 6);
        for (uint16_t b = 0; b < t.nblocks; b++) ch1 += (t.blocks[b].ch == 1);
        CHECK(ch1 == 4);
        CHECK(point_table_find(&t, MB_TAB_INPUT, 20) >= 0 && t.pts[point_table_find(&t, MB_TAB_INPUT, 20)].dev == 3);
    }

    /* 8. 一个通道最多 8 个设备：8 个可以，9 个拒绝；两个通道各 8 个也可以 */
    {
        pt_point_t p[16];
        for (uint16_t i = 0; i < 16; i++) { pt_point_t q = P(MB_TAB_COIL, (uint8_t)(1 + i / 8), (uint8_t)(1 + i % 8), i, 0); p[i] = q; }
        CHECK(parse(p, 16, &bad) == PT_OK);
        p[8].ch = 1;    /* 第 9 个设备挪到 ch1 */
        p[8].dev = 9;
        CHECK(parse(p, 16, &bad) == PT_ERR_DEVS);
    }

    CHECK(strcmp(pt_err_name(PT_ERR_DUP), "duplicate") == 0);

    printf("point_table: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
