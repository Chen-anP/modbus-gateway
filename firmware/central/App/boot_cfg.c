/*
 * boot_cfg.c - 配置区和开机判断，说明见 boot_cfg.h
 *
 * 为什么"没有配置 + 向量表合理"也跳（BOOT_JUMP_NO_CFG）：
 *   开发时用 Keil 直接下载 App，配置区是空的；如果这时留在 Bootloader，每次下载都得再走一遍升级，很不方便。
 * 代价：这时不查 CRC。所以第 7b 步升级时必须**最后**写 App 开头的 16 字节（栈指针 + 复位入口），
 *   中途断电时 App 开头还是 0xFF，向量表不合理 → BOOT_STAY_NO_APP，不会跳进半个 App。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "crc32.h"
#include "boot_cfg.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 结构体 ⇄ 32 字节（逐个字段按小端排，不依赖编译器的结构体布局） */
static void pack(const boot_cfg_t *c, uint8_t *b)
{
    wr32(&b[0], c->magic);   wr32(&b[4], c->app_len); wr32(&b[8], c->app_crc); wr32(&b[12], c->app_ver);
    wr32(&b[16], c->stay);   wr32(&b[20], c->rsv[0]); wr32(&b[24], c->rsv[1]); wr32(&b[28], c->self_crc);
}

int boot_cfg_read(const flash_io_t *io, uint32_t addr, boot_cfg_t *out)
{
    const uint8_t *b = io->ptr(addr);

    if (rd32(&b[0]) != BOOT_CFG_MAGIC)
        return 0;
    if (crc32_update(0, b, BOOT_CFG_SIZE - 4) != rd32(&b[28]))
        return 0;
    out->magic = rd32(&b[0]);   out->app_len = rd32(&b[4]); out->app_crc = rd32(&b[8]); out->app_ver = rd32(&b[12]);
    out->stay = rd32(&b[16]);   out->rsv[0] = rd32(&b[20]); out->rsv[1] = rd32(&b[24]); out->self_crc = rd32(&b[28]);
    return 1;
}

int boot_cfg_write(const flash_io_t *io, uint32_t addr, boot_cfg_t *cfg)
{
    uint8_t b[BOOT_CFG_SIZE];

    cfg->magic = BOOT_CFG_MAGIC;
    pack(cfg, b);
    cfg->self_crc = crc32_update(0, b, BOOT_CFG_SIZE - 4);
    wr32(&b[28], cfg->self_crc);
    if (io->erase(addr) != 0)
        return -1;
    return io->write(addr, b, BOOT_CFG_SIZE);
}

int boot_app_vectors_ok(const flash_io_t *io, uint32_t app_addr, uint32_t app_len,
                        uint32_t ram_start, uint32_t ram_end)
{
    const uint8_t *v = io->ptr(app_addr);
    uint32_t sp = rd32(&v[0]), reset = rd32(&v[4]);

    if (sp < ram_start || sp > ram_end || (sp & 7u) != 0)   /* 栈从高往低长，初始值可以正好等于 RAM 末尾 */
        return 0;
    if ((reset & 1u) == 0)                                  /* Cortex-M 只有 Thumb 状态，入口地址最低位必须是 1 */
        return 0;
    reset &= ~1u;
    if (reset < app_addr + 8u || reset >= app_addr + app_len)
        return 0;
    return 1;
}

boot_decision_t boot_cfg_decide(const flash_io_t *io, const boot_cfg_t *cfg, uint32_t app_addr, uint32_t app_max,
                                uint32_t ram_start, uint32_t ram_end)
{
    if (cfg != NULL && cfg->stay)
        return BOOT_STAY_REQUEST;
    if (cfg == NULL || cfg->app_len == 0)       /* 没有配置，或者配置里没有 App 信息（App 设过 stay 标志，但没升级过） */
        return boot_app_vectors_ok(io, app_addr, app_max, ram_start, ram_end) ? BOOT_JUMP_NO_CFG : BOOT_STAY_NO_APP;
    if (cfg->app_len < 8u || cfg->app_len > app_max)
        return BOOT_STAY_BAD_LEN;
    if (crc32_update(0, io->ptr(app_addr), cfg->app_len) != cfg->app_crc)
        return BOOT_STAY_BAD_CRC;
    if (!boot_app_vectors_ok(io, app_addr, cfg->app_len, ram_start, ram_end))
        return BOOT_STAY_BAD_VEC;
    return BOOT_JUMP;
}

const char *boot_decision_name(boot_decision_t d)
{
    static const char *const k[] = { "jump", "jump (no config)", "stay: requested", "stay: no app",
                                      "stay: bad length", "stay: bad crc", "stay: bad vectors" };
    return ((unsigned)d < sizeof(k) / sizeof(k[0])) ? k[d] : "?";
}
