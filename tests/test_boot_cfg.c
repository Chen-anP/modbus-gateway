/*
 * test_boot_cfg.c - 配置区读写，以及 Bootloader 开机判断的每一种情况
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I tests firmware/central/App/crc32.c
 *       firmware/central/App/boot_cfg.c tests/test_boot_cfg.c
 *
 * 假 Flash 前 32 KB 当 App 区，后面一个扇区当配置区（地址是假的，规则一样）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "fake_flash.h"
#include "crc32.h"
#include "boot_cfg.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define APP     0x08040000u
#define APP_MAX 0x8000u                 /* 测试用 32 KB */
#define CFG     (APP + APP_MAX)
#define RAM0    0x20000000u
#define RAM1    0x200A0000u

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* 做一个假 App：向量表（栈顶、复位入口），后面填点内容；写进 App 区。返回 CRC */
static uint32_t make_app(uint32_t len, uint32_t sp, uint32_t reset)
{
    static uint8_t img[APP_MAX];
    uint32_t i;
    for (i = 0; i < len; i++) img[i] = (uint8_t)(i * 13 + 5);
    wr32(&img[0], sp);
    wr32(&img[4], reset);
    for (i = 0; i < APP_MAX; i += FF_SECTOR) ff_erase(APP + i);
    ff_write(APP, img, (len + 15u) & ~15u);
    return crc32_update(0, img, len);
}

static boot_decision_t decide(void)
{
    boot_cfg_t c;
    int ok = boot_cfg_read(&ff_io, CFG, &c);
    return boot_cfg_decide(&ff_io, ok ? &c : NULL, APP, APP_MAX, RAM0, RAM1);
}

int main(void)
{
    boot_cfg_t c, r;
    uint32_t crc;
    long cut;

    ff_init(APP, APP_MAX + FF_SECTOR);

    /* 1. 全空：没有配置、App 区也是 0xFF → 留下 */
    CHECK(boot_cfg_read(&ff_io, CFG, &r) == 0);
    CHECK(decide() == BOOT_STAY_NO_APP);

    /* 2. 用调试器直接下载了 App（有向量表、没有配置）→ 不查 CRC 直接跳 */
    make_app(4096, RAM1, APP + 0x1C1);
    CHECK(decide() == BOOT_JUMP_NO_CFG);

    /* 3. 写一份正确的配置 → 跳 */
    crc = make_app(4096, RAM1, APP + 0x1C1);
    memset(&c, 0, sizeof(c));
    c.app_len = 4096; c.app_crc = crc; c.app_ver = 7;
    CHECK(boot_cfg_write(&ff_io, CFG, &c) == 0 && c.magic == BOOT_CFG_MAGIC);
    CHECK(boot_cfg_read(&ff_io, CFG, &r) == 1 && r.app_len == 4096 && r.app_crc == crc && r.app_ver == 7 && r.stay == 0);
    CHECK(decide() == BOOT_JUMP);

    /* 4. 上位机要求留下 → 留下；再清掉 → 跳。改配置要先擦：不会"写两次" */
    r.stay = 1;
    CHECK(boot_cfg_write(&ff_io, CFG, &r) == 0 && decide() == BOOT_STAY_REQUEST);
    r.stay = 0;
    CHECK(boot_cfg_write(&ff_io, CFG, &r) == 0 && decide() == BOOT_JUMP);
    CHECK(ff_double_write == 0 && ff_bad_args == 0);

    /* 5. App 里改了一个字节 → CRC 不对 → 留下 */
    ff_mem[1000] ^= 0x40;
    CHECK(decide() == BOOT_STAY_BAD_CRC);
    ff_mem[1000] ^= 0x40;
    CHECK(decide() == BOOT_JUMP);

    /* 6. 配置里的长度：0 = 没有 App 信息 → 和没有配置一样看向量表；4、超出 App 区 → 不合理 */
    r.app_len = 0;
    boot_cfg_write(&ff_io, CFG, &r);
    CHECK(decide() == BOOT_JUMP_NO_CFG);
    r.stay = 1;
    boot_cfg_write(&ff_io, CFG, &r);
    CHECK(decide() == BOOT_STAY_REQUEST);       /* 第 7a 步 App 发 0x0B00 后的样子 */
    r.stay = 0;
    r.app_len = 4;
    boot_cfg_write(&ff_io, CFG, &r);
    CHECK(decide() == BOOT_STAY_BAD_LEN);
    r.app_len = APP_MAX + 16;
    boot_cfg_write(&ff_io, CFG, &r);
    CHECK(decide() == BOOT_STAY_BAD_LEN);

    /* 7. 配置被改了一个字节 → 自己的 CRC 不对 → 当作没有配置（App 向量表合理 → 不查 CRC 跳） */
    r.app_len = 4096;
    boot_cfg_write(&ff_io, CFG, &r);
    ff_mem[CFG - APP + 8] ^= 1;
    CHECK(boot_cfg_read(&ff_io, CFG, &r) == 0);
    CHECK(decide() == BOOT_JUMP_NO_CFG);

    /* 8. 向量表检查：栈顶不在 RAM、不对齐；入口是偶数（不是 Thumb）、指到 App 外面、指到向量表本身 */
    CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 1);
    make_app(4096, 0x10000000u, APP + 0x1C1);  CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1 + 8,    APP + 0x1C1);  CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1 - 4,    APP + 0x1C1);  CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1,        APP + 0x1C0);  CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1,        APP + 4097);   CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1,        0x08000201u);  CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);
    make_app(4096, RAM1,        APP + 1);      CHECK(boot_app_vectors_ok(&ff_io, APP, 4096, RAM0, RAM1) == 0);

    /* 9. CRC 对、向量表不合理（比如下载了按 0x08000000 编译的固件）→ 留下 */
    crc = make_app(4096, RAM1, 0x08000201u);
    memset(&c, 0, sizeof(c));
    c.app_len = 4096; c.app_crc = crc;
    boot_cfg_write(&ff_io, CFG, &c);
    CHECK(decide() == BOOT_STAY_BAD_VEC);

    /* 10. 升级时"最后写向量表"的效果：App 开头 16 字节还是 0xFF、配置区是空的 → 留下，不会跳进半个 App */
    ff_init(APP, APP_MAX + FF_SECTOR);
    {
        static uint8_t body[2048];
        memset(body, 0x5A, sizeof(body));
        ff_write(APP + 16, body, sizeof(body));          /* 第 16 字节以后都写了，开头没写 */
    }
    CHECK(decide() == BOOT_STAY_NO_APP);

    /* 11. 写配置时断电：0 = 擦之前（旧配置还在），1 = 擦完没写（空），2 = 写了前 16 字节（自己的 CRC 不对 → 空），3 = 完成 */
    for (cut = 0; cut <= 3; cut++) {
        int ok;
        ff_init(APP, APP_MAX + FF_SECTOR);
        crc = make_app(4096, RAM1, APP + 0x1C1);
        memset(&c, 0, sizeof(c));
        c.app_len = 4096; c.app_crc = crc;
        boot_cfg_write(&ff_io, CFG, &c);                /* 旧配置：stay = 0 */
        c.stay = 1;
        ff_power_budget = cut;
        boot_cfg_write(&ff_io, CFG, &c);                /* 新配置：stay = 1 */
        ff_power_on();
        ok = boot_cfg_read(&ff_io, CFG, &r);
        if (cut == 0)      CHECK(ok == 1 && r.stay == 0);
        else if (cut == 3) CHECK(ok == 1 && r.stay == 1);
        else               CHECK(ok == 0);
    }

    /* 12. 名字 */
    CHECK(strcmp(boot_decision_name(BOOT_JUMP), "jump") == 0);
    CHECK(strcmp(boot_decision_name((boot_decision_t)99), "?") == 0);

    printf("boot_cfg: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
