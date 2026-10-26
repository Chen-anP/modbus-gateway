/*
 * test_fwd.c - 转发：用一个"假传感器"模拟课程 F030 Bootloader 的真实行为
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c
 *       firmware/central/App/crc32.c firmware/central/App/fwd.c tests/test_fwd.c
 *
 * 假传感器照着 bootloader.c 的 burn_firmware() 写：文件头 → 擦 App 区和配置区；之后每包接着写、不看序号；
 * 写满文件长度 → 配置区"App 有效"。往没擦的地方写会被记成"写错位"。还能模拟"执行了但回应丢了"。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "mb_rtu.h"
#include "crc32.h"
#include "fwd.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- 假传感器（通道 2，地址 3） */
static struct {
    int      in_boot;
    uint8_t  flash[SENSOR_APP_MAX];
    int      cfg_valid;                 /* 配置区：App 有效 */
    uint32_t file_len, recv, addr;
    int      erases, writes, bad_writes;
    int      app_erased_while_running;  /* App 在运行时收到了文件（会把自己擦掉） */
    int      calls;                     /* 收到的请求数 */
    int      drop_reply_at;             /* 第几个请求执行了但不回应（0 = 不丢） */
    int      lost_at;                   /* 第几个请求根本没收到 */
} S;
static int g_wrong_addr, g_wrong_file_no;     /* 整个测试累计，不随 sensor_reset 清零 */

static void sensor_reset(void)
{
    memset(&S, 0, sizeof(S));
    memset(S.flash, 0x5A, sizeof(S.flash));     /* 里面是旧 App */
    S.cfg_valid = 1;
}

/* 传感器执行一个请求；返回 1 = 回应了 */
static int sensor_exec(const uint8_t *req, uint16_t len)
{
    S.calls++;
    if (S.calls == S.lost_at)
        return 0;
    if (req[0] != 3) g_wrong_addr++;
    if (req[1] == MB_FC_WRITE_REG) {
        uint16_t v = (uint16_t)((req[4] << 8) | req[5]);
        if (v == 0x55) S.in_boot = 1;
        if (v == 0xAA && S.cfg_valid) S.in_boot = 0;
    } else if (req[1] == MB_FC_WRITE_FILE_RECORD) {
        uint16_t rec = (uint16_t)((req[6] << 8) | req[7]);
        if (((req[4] << 8) | req[5]) != SENSOR_FILE_NO) g_wrong_file_no++;
        if (!S.in_boot)
            S.app_erased_while_running = 1;
        if (rec == 0) {                                     /* burn_firmware：文件头 */
            S.file_len = ((uint32_t)req[14] << 24) | ((uint32_t)req[15] << 16) | ((uint32_t)req[16] << 8) | req[17];
            memset(S.flash, 0xFF, (S.file_len + 2047u) / 2048u * 2048u);
            S.cfg_valid = 0;
            S.recv = S.addr = 0;
            S.erases++;
        } else {                                            /* 不看序号，接着写 */
            int cur = req[2] - 7, i;
            for (i = 0; i < cur; i++) {
                if (S.addr + (uint32_t)i >= sizeof(S.flash)) break;
                if (S.flash[S.addr + i] != 0xFF) S.bad_writes++;
                S.flash[S.addr + i] = req[10 + i];
            }
            S.addr += (uint32_t)cur;
            S.recv += (uint32_t)cur;
            S.writes++;
            if (S.recv >= S.file_len) S.cfg_valid = 1;
        }
    }
    (void)len;
    return S.calls != S.drop_reply_at;
}

/* 和板子上的 mb_master 一样：没回应就按 retries 重发（传感器会再执行一次） */
static int g_last_ch;
static int fake_transact(void *ctx, uint8_t ch, const uint8_t *req, uint16_t len, uint32_t timeout, uint8_t retries, mb_ex_t *ex)
{
    uint8_t a;
    (void)ctx; (void)timeout;
    *ex = MB_EX_NONE;
    g_last_ch = ch;
    for (a = 0; a <= retries; a++)
        if (sensor_exec(req, len))
            return MB_OK;
    return MB_ERR_TIMEOUT;
}

static uint16_t g_target, g_err;
static uint16_t get_target(void *c) { (void)c; return g_target; }
static void set_err(void *c, uint16_t e) { (void)c; g_err = e; }

/* ---------------------------------------------------------------- 上位机这一侧 */
static fwd_t F;
static uint8_t g_rsp[MB_ADU_MAX];

static int ask(const uint8_t *req, uint16_t len, int *handled)
{
    mb_ex_t ex;
    uint16_t n = fwd_handle(&F, 1, req, len, g_rsp, handled);
    if (!*handled) return 99;
    return mb_parse_rsp(req, len, g_rsp, n, NULL, 0, &ex) == MB_OK ? 0 : g_rsp[2];   /* 0 = 成功，否则异常码 */
}

static int cmd(uint16_t v)
{
    uint8_t req[MB_ADU_MAX]; int h;
    uint16_t n = mb_build_write_single(req, 1, MB_TAB_HOLDING, FWD_SYS_TARGET_CMD, v);
    return ask(req, n, &h);
}

static int rec(uint16_t file_no, uint16_t r, const uint8_t *d, uint16_t n)
{
    uint8_t req[MB_ADU_MAX]; int h;
    uint16_t len = mb_build_file_record(req, 1, file_no, r, d, n);
    return ask(req, len, &h);
}

static void hdr(uint8_t *h, uint32_t len, uint32_t crc)
{
    memset(h, 0, 32);
    h[3] = 1;
    h[4] = (uint8_t)(len >> 24); h[5] = (uint8_t)(len >> 16); h[6] = (uint8_t)(len >> 8); h[7] = (uint8_t)len;
    h[8] = 0x08; h[9] = 0x02;
    h[12] = (uint8_t)(crc >> 24); h[13] = (uint8_t)(crc >> 16); h[14] = (uint8_t)(crc >> 8); h[15] = (uint8_t)crc;
    memcpy(&h[16], "sensor.bin", 10);
}

#define FN 0x1203u      /* 通道 2、地址 3 */

/* 发整个文件：返回第一个失败包的结果（0 = 全部成功），*at = 失败在第几包 */
static int send_file(const uint8_t *img, uint32_t len, uint32_t crc, int *at)
{
    uint8_t h[32];
    uint32_t off;
    uint16_t r = 1;
    int rc;

    hdr(h, len, crc);
    *at = 0;
    if ((rc = rec(FN, 0, h, 32)) != 0) return rc;
    for (off = 0; off < len; off += 224, r++) {
        uint16_t n = (uint16_t)((len - off) < 224 ? (len - off) : 224);
        *at = r;
        if ((rc = rec(FN, r, &img[off], n)) != 0) return rc;
    }
    return 0;
}

static uint8_t img[6001];

static void make_img(uint32_t len, uint32_t entry)
{
    uint32_t i;
    for (i = 0; i < len; i++) img[i] = (uint8_t)(i * 7 + 3);
    img[0] = 0x00; img[1] = 0x10; img[2] = 0x00; img[3] = 0x20;                 /* SP = 0x20001000 */
    img[4] = (uint8_t)entry; img[5] = (uint8_t)(entry >> 8); img[6] = (uint8_t)(entry >> 16); img[7] = (uint8_t)(entry >> 24);
}

int main(void)
{
    uint32_t crc;
    int at, h;

    fwd_init(&F, fake_transact, NULL, get_target, set_err);
    sensor_reset();

    /* 1. 不是转发的请求：读系统区、点表文件（0x0000）、中控自己的固件（0x1000）、CRC 错 → 不处理 */
    {
        uint8_t req[MB_ADU_MAX]; uint8_t d[32] = {0};
        uint16_t n = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF000, 4);
        CHECK(ask(req, n, &h) == 99 && h == 0);
        n = mb_build_file_record(req, 1, 0x0000, 0, d, 32);  CHECK(ask(req, n, &h) == 99);
        n = mb_build_file_record(req, 1, 0x1000, 0, d, 32);  CHECK(ask(req, n, &h) == 99);
        n = mb_build_file_record(req, 1, 0x1303, 0, d, 32);  CHECK(ask(req, n, &h) == 99);   /* 通道 3 不存在 */
        n = mb_build_write_single(req, 1, MB_TAB_HOLDING, FWD_SYS_TARGET_CMD, 0x55);
        req[n - 1] ^= 1;                                    CHECK(ask(req, n, &h) == 99);
        n = mb_build_write_single(req, 2, MB_TAB_HOLDING, FWD_SYS_TARGET_CMD, 0x55);
        CHECK(ask(req, n, &h) == 99);                       /* 不是发给中控的 */
    }

    /* 2. 命令：没设目标 → 异常 04、0x0601；设好 → 0x55 让传感器进 Bootloader */
    CHECK(cmd(0x55) == 4 && g_err == FWD_ERR_TARGET);
    g_target = 0x0203;
    CHECK(cmd(0x55) == 0 && S.in_boot == 1 && g_last_ch == 2 && F.cmds_fwd == 1);

    /* 3. 正常升级 5000 字节：文件头先不转发；第 1 包到了才擦 Flash；全部写对，配置区有效 */
    make_img(5000, SENSOR_APP_ADDR + 0xC1);
    crc = crc32_update(0, img, 5000);
    {
        uint8_t hh[32];
        hdr(hh, 5000, crc);
        CHECK(rec(FN, 0, hh, 32) == 0 && F.state == FWD_HDR_HELD && S.erases == 0 && S.calls == 1);
    }
    CHECK(send_file(img, 5000, crc, &at) == 0 && F.state == FWD_DONE);
    CHECK(memcmp(S.flash, img, 5000) == 0 && S.cfg_valid && S.bad_writes == 0 && !S.app_erased_while_running);
    CHECK(S.erases == 1 && S.writes == 23 && F.records_fwd == 23);           /* 5000 / 224 = 22.3 → 23 包 */

    /* 4. 上位机重发了最后一包（它没收到回应）：回成功，不再转发（传感器那边不会写错位） */
    CHECK(rec(FN, 23, &img[22 * 224], 5000 - 22 * 224) == 0 && F.dup_skipped == 1 && S.writes == 23 && S.bad_writes == 0);

    /* 5. 0xAA：传感器启动新 App */
    CHECK(cmd(0xAA) == 0 && S.in_boot == 0);

    /* 6. 发错文件（复位入口在 0x08040000 之后：中控的固件）：第 1 包被拒，传感器的 Flash 一点没动 */
    sensor_reset(); S.in_boot = 1; g_err = 0;
    make_img(5000, 0x08040201u);
    crc = crc32_update(0, img, 5000);
    CHECK(send_file(img, 5000, crc, &at) == 3 && at == 1 && g_err == FWD_ERR_VECTORS);
    CHECK(S.erases == 0 && S.writes == 0 && S.cfg_valid == 1);              /* 旧 App 还在 */
    CHECK(rec(FN, 2, img, 224) == 4 && g_err == FWD_ERR_BROKEN);           /* 之后的包都拒绝 */

    /* 7. 文件头里的 CRC 不对：前面都转发了，最后一包不发 → 传感器没写"App 有效"，留在 Bootloader */
    sensor_reset(); S.in_boot = 1; g_err = 0;
    make_img(5000, SENSOR_APP_ADDR + 0xC1);
    crc = crc32_update(0, img, 5000);
    CHECK(send_file(img, 5000, crc ^ 1, &at) == 4 && at == 23 && g_err == FWD_ERR_CRC);
    CHECK(S.writes == 22 && S.cfg_valid == 0);

    /* 8. 中间一包"传感器写了、回应丢了"：作废，后面的包拒绝；从文件头重来 → 成功，Flash 正确 */
    sensor_reset(); S.in_boot = 1; g_err = 0;
    S.drop_reply_at = 1 + 10;                                               /* 第 1 个请求是文件头，再往后第 10 包 */
    CHECK(send_file(img, 5000, crc, &at) == 4 && at == 10 && g_err == FWD_ERR_NO_REPLY);
    CHECK(rec(FN, 11, &img[10 * 224], 224) == 4 && g_err == FWD_ERR_BROKEN);
    CHECK(rec(FN, 10, &img[9 * 224], 224) == 4);                            /* 重发失败的那一包也不行：不知道传感器写没写 */
    S.drop_reply_at = 0;
    CHECK(send_file(img, 5000, crc, &at) == 0 && memcmp(S.flash, img, 5000) == 0 && S.bad_writes == 0 && S.cfg_valid);

    /* 9. 文件头的回应丢了：自动重发一次（传感器多擦一遍，没关系） */
    sensor_reset(); S.in_boot = 1;
    S.drop_reply_at = 1;
    CHECK(send_file(img, 5000, crc, &at) == 0 && S.erases == 2 && memcmp(S.flash, img, 5000) == 0);

    /* 10. 传感器完全没回应（没接、地址错）：文件头重试后还是没有 → 异常 04、0x0602，传感器不受影响 */
    sensor_reset(); S.in_boot = 1; S.lost_at = 1; g_err = 0;
    {
        int i;
        S.drop_reply_at = 2;                                                /* 第 2 次（重试）也不回 */
        CHECK(send_file(img, 5000, crc, &at) == 4 && at == 1 && g_err == FWD_ERR_NO_REPLY);
        (void)i;
    }

    /* 11. 跳包、没有文件头就来数据、长度超出 */
    sensor_reset(); S.in_boot = 1; fwd_init(&F, fake_transact, NULL, get_target, set_err);
    CHECK(rec(FN, 1, img, 224) == 3 && g_err == FWD_ERR_SEQUENCE);          /* 没有文件头 */
    {
        uint8_t hh[32];
        hdr(hh, 5000, crc);
        CHECK(rec(FN, 0, hh, 32) == 0);
        CHECK(rec(FN, 1, img, 224) == 0);
        CHECK(rec(FN, 3, &img[448], 224) == 3 && g_err == FWD_ERR_SEQUENCE); /* 跳过了第 2 包 */
        hdr(hh, SENSOR_APP_MAX + 1, crc);
        CHECK(rec(FN, 0, hh, 32) == 3 && g_err == FWD_ERR_LENGTH);           /* 比传感器 App 区还大 */
        CHECK(rec(FN, 0, hh, 16) == 3 && g_err == FWD_ERR_REQUEST);          /* 文件头不是 32 字节 */
    }

    /* 12. 奇数长度（最后一包补了一个 0）：CRC 只算文件本身的字节 */
    sensor_reset(); S.in_boot = 1;
    make_img(4999, SENSOR_APP_ADDR + 0xC1);
    crc = crc32_update(0, img, 4999);
    CHECK(send_file(img, 4999, crc, &at) == 0 && F.state == FWD_DONE && memcmp(S.flash, img, 4999) == 0);

    /* 13. 整个过程中传感器从来没有在 App 运行时收到文件 */
    CHECK(!S.app_erased_while_running);
    CHECK(g_wrong_addr == 0 && g_wrong_file_no == 0);        /* 转发出去的地址是 3、file_no 是 1 */
    CHECK(strcmp(fwd_state_name(FWD_BROKEN), "broken") == 0);

    printf("fwd: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
