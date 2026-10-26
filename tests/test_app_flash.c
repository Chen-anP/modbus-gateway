/*
 * test_app_flash.c - Bootloader 写新固件：写之前的检查，以及在每一次 Flash 操作前断电
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I firmware/boot/App -I tests
 *       firmware/central/App/crc32.c firmware/central/App/boot_cfg.c firmware/boot/App/app_flash.c
 *       tests/test_app_flash.c
 *
 * 假 Flash：App 区 32 KB（4 个扇区）+ 配置区 1 个扇区，地址是假的，规则和真芯片一样。
 * 断电测试检查的是：重新上电后 Bootloader 的判断，要么"留下"，要么跳进一个**完整的**旧 App 或新 App。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "fake_flash.h"
#include "crc32.h"
#include "boot_cfg.h"
#include "app_flash.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define APP     0x08040000u
#define APP_MAX 0x8000u
#define CFG     (APP + APP_MAX)
#define RAM0    0x20000000u
#define RAM1    0x200A0000u

static const af_layout_t L = { APP, APP_MAX, CFG, RAM0, RAM1 };

static uint8_t old_img[12000], new_img[20000];

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void make(uint8_t *img, uint32_t len, uint8_t seed)
{
    uint32_t i;
    for (i = 0; i < len; i++) img[i] = (uint8_t)(i * 31 + seed);
    wr32(&img[0], RAM1);
    wr32(&img[4], APP + 0x201);
}

/* 用调试器下载旧 App：直接写进 App 区（不经过 app_flash） */
static void flash_old_app(int with_cfg, uint32_t stay)
{
    uint32_t i;
    ff_init(APP, APP_MAX + FF_SECTOR);
    for (i = 0; i < APP_MAX; i += FF_SECTOR) ff_erase(APP + i);
    ff_write(APP, old_img, sizeof(old_img) & ~15u);
    {
        uint8_t blk[16];
        memset(blk, 0xFF, 16);
        memcpy(blk, &old_img[sizeof(old_img) & ~15u], sizeof(old_img) % 16);
        if (sizeof(old_img) % 16) ff_write(APP + (sizeof(old_img) & ~15u), blk, 16);
    }
    if (with_cfg) {
        boot_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.app_len = sizeof(old_img);
        c.app_crc = crc32_update(0, old_img, sizeof(old_img));
        c.stay = stay;
        boot_cfg_write(&ff_io, CFG, &c);
    }
    ff_erases = ff_writes = 0;
}

static boot_decision_t decide(boot_cfg_t *c)
{
    int ok = boot_cfg_read(&ff_io, CFG, c);
    return boot_cfg_decide(&ff_io, ok ? c : NULL, APP, APP_MAX, RAM0, RAM1);
}

static int app_is(const uint8_t *img, uint32_t len) { return memcmp(ff_ptr(APP), img, len) == 0; }

int main(void)
{
    uint32_t new_crc;
    boot_cfg_t c;
    int initial;

    make(old_img, sizeof(old_img), 3);
    make(new_img, sizeof(new_img), 7);
    new_crc = crc32_update(0, new_img, sizeof(new_img));

    /* 1. 写之前的检查 */
    CHECK(app_flash_check(&L, new_img, sizeof(new_img), APP) == AF_OK);
    CHECK(app_flash_check(&L, new_img, sizeof(new_img), 0x08000000u) == AF_ERR_ADDR);   /* 按旧地址编译的 */
    CHECK(app_flash_check(&L, new_img, APP_MAX + 16, APP) == AF_ERR_SIZE);
    CHECK(app_flash_check(&L, new_img, 8, APP) == AF_ERR_SIZE);
    {
        static uint8_t bad[64];
        memcpy(bad, new_img, sizeof(bad));
        wr32(&bad[4], 0x08000201u);                     /* 复位入口指到 Bootloader 区 */
        CHECK(app_flash_check(&L, bad, sizeof(bad), APP) == AF_ERR_VECTORS);
        wr32(&bad[4], APP + 0x201);
        wr32(&bad[0], 0xFFFFFFFFu);                     /* 栈指针不在 RAM */
        CHECK(app_flash_check(&L, bad, sizeof(bad), APP) == AF_ERR_VECTORS);
    }

    /* 2. 正常升级：旧 App + stay = 1（上位机发过 0x0B00） */
    flash_old_app(1, 1);
    CHECK(decide(&c) == BOOT_STAY_REQUEST);
    CHECK(app_flash_program(&ff_io, &L, new_img, sizeof(new_img), new_crc, 8) == AF_OK);
    CHECK(app_is(new_img, sizeof(new_img)));
    CHECK(ff_double_write == 0 && ff_bad_args == 0);
    CHECK(ff_erases == 2 + 3);                         /* 配置区 2 次 + App 3 个扇区（20000 字节） */
    CHECK(decide(&c) == BOOT_STAY_REQUEST && c.app_len == sizeof(new_img) && c.app_crc == new_crc && c.app_ver == 8);
    c.stay = 0;                                         /* 上位机发 0x0A00 */
    boot_cfg_write(&ff_io, CFG, &c);
    CHECK(decide(&c) == BOOT_JUMP);

    /* 3. 长度不是 16 的倍数：最后一块补 0xFF，CRC 只算 len 字节 */
    flash_old_app(1, 1);
    CHECK(app_flash_program(&ff_io, &L, new_img, 1000 + 5, crc32_update(0, new_img, 1005), 9) == AF_OK);
    CHECK(app_is(new_img, 1005) && ff_mem[1005] == 0xFF && ff_mem[1007] == 0xFF);

    /* 4. CRC 给错了：写完核对不过 → verify，配置区 app_len 还是 0、stay 1 → 留下 */
    flash_old_app(1, 1);
    CHECK(app_flash_program(&ff_io, &L, new_img, sizeof(new_img), new_crc ^ 1, 8) == AF_ERR_VERIFY);
    CHECK(decide(&c) == BOOT_STAY_REQUEST && c.app_len == 0);

    /* 5. 每一次 Flash 操作前断电。两种起点：有配置（stay = 1）、没有配置（Keil 直接下载的旧 App） */
    for (initial = 0; initial < 2; initial++) {
        long total, cut;
        int n_stay = 0, n_old = 0, n_new = 0, n_bad = 0;

        flash_old_app(initial == 0, 1);
        ff_power_budget = -1;
        app_flash_program(&ff_io, &L, new_img, sizeof(new_img), new_crc, 8);
        total = (long)(ff_erases + ff_writes);          /* 一共多少次操作 */

        for (cut = 0; cut <= total; cut++) {
            boot_decision_t d;
            af_err_t e;
            flash_old_app(initial == 0, 1);
            ff_power_budget = cut;
            e = app_flash_program(&ff_io, &L, new_img, sizeof(new_img), new_crc, 8);
            ff_power_on();
            if (cut == total) CHECK(e == AF_OK);
            d = decide(&c);
            if (d == BOOT_JUMP || d == BOOT_JUMP_NO_CFG) {
                if (app_is(old_img, sizeof(old_img))) n_old++;
                else if (app_is(new_img, sizeof(new_img))) n_new++;
                else { n_bad++; printf("  start %d cut %ld: jump into a broken app (%s)\n", initial, cut, boot_decision_name(d)); }
            } else {
                n_stay++;
            }
        }
        printf("  power cut (%s): %ld ops, stay %d, jump old %d, jump new %d, jump broken %d\n",
               initial == 0 ? "config stay=1" : "no config", total, n_stay, n_old, n_new, n_bad);
        CHECK(n_bad == 0);
        CHECK(n_stay > 0);
    }

    CHECK(strcmp(af_err_name(AF_ERR_VERIFY), "verify") == 0);
    printf("app_flash: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
