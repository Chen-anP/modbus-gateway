/*
 * test_pt_store.c - 点表存进 Flash：存、读、清、坏数据，以及在每一个可能的位置断电
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I tests firmware/central/App/crc32.c
 *       firmware/central/App/pt_store.c tests/test_pt_store.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "fake_flash.h"
#include "flash_layout.h"
#include "pt_store.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define A FL_PT_ADDR

static uint8_t t_old[176], t_new[64];     /* 22 个点 / 8 个点，和默认表、演示表一样大 */

int main(void)
{
    const uint8_t *d;
    uint32_t len, seq, i;
    long cut;

    for (i = 0; i < sizeof(t_old); i++) t_old[i] = (uint8_t)(i * 7 + 1);
    for (i = 0; i < sizeof(t_new); i++) t_new[i] = (uint8_t)(0xA0 ^ i);

    /* 1. 全新的芯片：是空的 */
    ff_init(A, 2 * FF_SECTOR);
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_EMPTY);

    /* 2. 存进去、读出来，内容和长度一样；只擦了 1 次，写了 2（头）+ 11（176 字节）次 */
    CHECK(pt_store_save(&ff_io, A, t_old, sizeof(t_old), 1) == PTS_OK);
    CHECK(ff_erases == 1 && ff_writes == 2 + 11 && ff_double_write == 0 && ff_bad_args == 0);
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_OK);
    CHECK(len == sizeof(t_old) && memcmp(d, t_old, len) == 0 && seq == 1);

    /* 3. 长度不是 16 的倍数（8 个点 = 64 可以，故意用 40）：最后一块补 0xFF，读回来长度还是 40 */
    CHECK(pt_store_save(&ff_io, A, t_new, 40, 2) == PTS_OK);
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_OK && len == 40 && memcmp(d, t_new, 40) == 0 && seq == 2);
    CHECK(ff_mem[32 + 40] == 0xFF && ff_mem[32 + 47] == 0xFF);

    /* 4. 存第二次之前要先擦：不会"写两次" */
    CHECK(pt_store_save(&ff_io, A, t_old, sizeof(t_old), 3) == PTS_OK && ff_double_write == 0);

    /* 5. 太大、长度 0：拒绝，Flash 不动 */
    {
        static uint8_t big[PT_STORE_MAX_DATA + 8];
        unsigned e = ff_erases;
        CHECK(pt_store_save(&ff_io, A, big, sizeof(big), 4) == PTS_TOO_BIG);
        CHECK(pt_store_save(&ff_io, A, big, 0, 4) == PTS_TOO_BIG);
        CHECK(ff_erases == e);
        CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_OK && seq == 3);
    }

    /* 6. 数据被改了一个字节（Flash 坏了）：头是好的，数据 CRC 不对 */
    ff_mem[32 + 5] ^= 0x01;
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_BAD_DATA);
    ff_mem[32 + 5] ^= 0x01;
    /* 头里的长度被改了：头自己的 CRC 不对 → 当作空 */
    ff_mem[8] ^= 0x10;
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_EMPTY);
    ff_mem[8] ^= 0x10;
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_OK);

    /* 7. 清掉：变成空的 */
    CHECK(pt_store_clear(&ff_io, A) == PTS_OK);
    CHECK(pt_store_load(&ff_io, A, &d, &len, &seq) == PTS_EMPTY);

    /* 8. 断电：旧表在 Flash 里，存新表时在第 cut 次操作前断电（0 = 擦之前，1 = 擦完 …）。
     *    重新上电后读出来只能是：旧表（还没擦）、空（擦了但新表的头没写完）、完整的新表——绝不是半张表 */
    {
        int n_old = 0, n_empty = 0, n_new = 0, n_other = 0;
        for (cut = 0; cut <= 1 + 4 + 2; cut++) {        /* 1 次擦 + 4 次写数据（64 字节）+ 2 次写头 */
            pts_result_t r;
            ff_init(A, 2 * FF_SECTOR);
            pt_store_save(&ff_io, A, t_old, sizeof(t_old), 1);
            ff_power_budget = cut;
            r = pt_store_save(&ff_io, A, t_new, sizeof(t_new), 2);
            CHECK((r == PTS_OK) == (cut == 7));         /* 只有操作全部完成才算成功 */
            ff_power_on();
            r = pt_store_load(&ff_io, A, &d, &len, &seq);
            if (r == PTS_OK && seq == 1 && len == sizeof(t_old) && memcmp(d, t_old, len) == 0) n_old++;
            else if (r == PTS_EMPTY) n_empty++;
            else if (r == PTS_OK && seq == 2 && len == sizeof(t_new) && memcmp(d, t_new, len) == 0) n_new++;
            else { n_other++; printf("  cut %ld: %s\n", cut, pts_name(r)); }
        }
        printf("  power cut: old %d, empty %d, new %d, other %d\n", n_old, n_empty, n_new, n_other);
        CHECK(n_old == 1);          /* 擦之前断电 */
        CHECK(n_empty == 6);        /* 擦完 ~ 头写完之前 */
        CHECK(n_new == 1);          /* 全部完成 */
        CHECK(n_other == 0);
    }

    /* 9. 擦除失败（比如地址不对）：报 Flash 错误 */
    ff_init(A, 2 * FF_SECTOR);
    CHECK(pt_store_save(&ff_io, A + 16, t_new, sizeof(t_new), 1) == PTS_FLASH_ERR && ff_bad_args == 1);

    printf("pt_store: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
