/*
 * test_boot_proto.c - Bootloader 的 Modbus 从机：用真实的请求帧走 mb_slave_handle()，
 *                     从读寄存器、收固件、写 Flash 到 0x0A00 走一遍完整的升级
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I firmware/boot/App -I tests
 *       firmware/central/App/mb_rtu.c firmware/central/App/crc32.c firmware/central/App/file_rx.c
 *       firmware/central/App/boot_cfg.c firmware/boot/App/app_flash.c firmware/boot/App/boot_proto.c
 *       tests/test_boot_proto.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "fake_flash.h"
#include "mb_rtu.h"
#include "crc32.h"
#include "boot_cfg.h"
#include "boot_proto.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define APP     0x08040000u
#define APP_MAX 0x8000u
#define CFG     (APP + APP_MAX)

static const af_layout_t L = { APP, APP_MAX, CFG, 0x20000000u, 0x200A0000u };
static boot_proto_t bp;
static mb_slave_ops_t ops;
static uint8_t rxbuf[APP_MAX];
static uint8_t img[5000];

static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int ask_read(uint16_t addr, uint16_t n, uint16_t *v, mb_ex_t *ex)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t q = mb_build_read(req, 1, MB_TAB_HOLDING, addr, n);
    uint16_t r = mb_slave_handle(1, &ops, req, q, rsp);
    return r ? mb_parse_rsp(req, q, rsp, r, v, 16, ex) : 99;
}

static int ask_cmd(uint16_t value, mb_ex_t *ex)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t q = mb_build_write_single(req, 1, MB_TAB_HOLDING, BP_REG_CMD, value);
    uint16_t r = mb_slave_handle(1, &ops, req, q, rsp);
    return r ? mb_parse_rsp(req, q, rsp, r, NULL, 0, ex) : 99;
}

static int send_rec(uint16_t file_no, uint16_t rec, const uint8_t *d, uint16_t len, mb_ex_t *ex)
{
    uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
    uint16_t n = mb_build_file_record(req, 1, file_no, rec, d, len);
    uint16_t r = mb_slave_handle(1, &ops, req, n, rsp);
    return r ? mb_parse_rsp(req, n, rsp, r, NULL, 0, ex) : 99;
}

/* 和上位机工具一样发一个固件文件 */
static int send_fw(uint16_t file_no, uint32_t load, uint32_t crc, uint32_t ver, mb_ex_t *ex)
{
    uint8_t hdr[FILE_HDR_SIZE] = { 0 };
    uint16_t rec = 1;
    uint32_t off;
    int rc;

    put32(&hdr[0], ver); put32(&hdr[4], sizeof(img)); put32(&hdr[8], load); put32(&hdr[12], crc);
    memcpy(&hdr[16], "central.bin", 11);
    if ((rc = send_rec(file_no, 0, hdr, FILE_HDR_SIZE, ex)) != MB_OK)
        return rc;
    for (off = 0; off < sizeof(img); off += FILE_REC_DATA) {
        uint16_t n = (uint16_t)((sizeof(img) - off) < FILE_REC_DATA ? (sizeof(img) - off) : FILE_REC_DATA);
        if ((rc = send_rec(file_no, rec++, &img[off], n, ex)) != MB_OK)
            return rc;
    }
    return MB_OK;
}

/* 主循环做的事 */
static void run_program(void)
{
    if (boot_proto_take_action(&bp) == BP_ACT_PROGRAM)
        boot_proto_program_done(&bp, app_flash_program(&ff_io, &L, bp.rx.buf, bp.rx.hdr.length,
                                                       bp.rx.hdr.crc32, bp.rx.hdr.version));
}

int main(void)
{
    uint16_t v[16];
    mb_ex_t ex;
    uint32_t i, crc;
    boot_cfg_t c;

    for (i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 7 + 1);
    wr32le(&img[0], 0x200A0000u);
    wr32le(&img[4], APP + 0x1C1);
    crc = crc32_update(0, img, sizeof(img));

    ff_init(APP, APP_MAX + FF_SECTOR);
    boot_proto_init(&bp, rxbuf, sizeof(rxbuf), &L, 0x0100, BOOT_STAY_REQUEST, 7);
    boot_proto_ops(&bp, &ops);

    /* 1. 系统区：CMD 0、STATE 1、Bootloader 版本、点数 0、LAST_ERR 0、原因、UPG_STATE 0、APP_VER */
    CHECK(ask_read(0xF000, 8, v, &ex) == MB_OK);
    CHECK(v[0] == 0 && v[1] == 1 && v[2] == 0x0100 && v[3] == 0 && v[4] == 0 && v[5] == BOOT_STAY_REQUEST && v[6] == 0 && v[7] == 7);
    CHECK(ask_read(0xF008, 1, v, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(ask_read(0xF007, 2, v, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(ask_read(0x0000, 1, v, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 2. 命令：0x0B00 接受、什么都不做；0x1234 异常 03；只读寄存器不能写 */
    CHECK(ask_cmd(BP_CMD_ENTER_BOOT, &ex) == MB_OK && boot_proto_take_action(&bp) == BP_ACT_NONE);
    CHECK(ask_cmd(0x1234, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_VALUE);
    {
        uint8_t req[MB_ADU_MAX], rsp[MB_ADU_MAX];
        uint16_t q = mb_build_write_single(req, 1, MB_TAB_HOLDING, 0xF001, 0);
        uint16_t r = mb_slave_handle(1, &ops, req, q, rsp);
        CHECK(mb_parse_rsp(req, q, rsp, r, NULL, 0, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    }

    /* 3. file_no 不对（点表 0x0000、传感器 0x1203）→ 异常 02 */
    CHECK(send_fw(0x0000, APP, crc, 8, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(send_fw(0x1203, APP, crc, 8, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 4. 按旧地址 0x08000000 编译的固件（文件头加载地址不对）：最后一包回异常 04，LAST_ERR 0x0501，Flash 一点没动 */
    CHECK(send_fw(FILE_NO_CENTRAL_FW, 0x08000000u, crc, 8, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_DEVICE_FAILURE);
    CHECK(bp.last_err == (BP_ERR_UPGRADE | AF_ERR_ADDR) && bp.upg_state == BP_UPG_FAILED);
    CHECK(boot_proto_take_action(&bp) == BP_ACT_NONE && ff_erases == 0 && ff_writes == 0);

    /* 5. CRC 不对：异常 04，LAST_ERR 0x0305，Flash 一点没动 */
    CHECK(send_fw(FILE_NO_CENTRAL_FW, APP, crc ^ 1, 8, &ex) == MB_ERR_EXCEPTION);
    CHECK(bp.last_err == (BP_ERR_FILE | FRX_ERR_CRC) && bp.upg_state == BP_UPG_FAILED);
    CHECK(ff_erases == 0 && ff_writes == 0);

    /* 6. 正常升级：收齐 → UPG_STATE 2、要写 Flash；写完 → 3、APP_VER 8、LAST_ERR 0 */
    CHECK(send_fw(FILE_NO_CENTRAL_FW, APP, crc, 8, &ex) == MB_OK);
    CHECK(bp.upg_state == BP_UPG_PROGRAMMING && bp.last_err == 0);
    /* 正在写 Flash 时来了 0x15（板子上不会发生）→ 异常 06 忙 */
    CHECK(send_rec(FILE_NO_CENTRAL_FW, 0, img, FILE_HDR_SIZE, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_BUSY);
    run_program();
    CHECK(ask_read(0xF004, 4, v, &ex) == MB_OK && v[0] == 0 && v[2] == BP_UPG_DONE && v[3] == 8);
    CHECK(memcmp(ff_ptr(APP), img, sizeof(img)) == 0);
    CHECK(boot_cfg_read(&ff_io, CFG, &c) == 1 && c.stay == 1 && c.app_len == sizeof(img) && c.app_crc == crc);

    /* 7. 0x0A00 → 主循环要清 stay、重启 */
    CHECK(ask_cmd(BP_CMD_RUN_APP, &ex) == MB_OK && boot_proto_take_action(&bp) == BP_ACT_RUN_APP);
    CHECK(ask_cmd(BP_CMD_REBOOT, &ex) == MB_OK && boot_proto_take_action(&bp) == BP_ACT_REBOOT);
    CHECK(ask_cmd(BP_CMD_FORGET_APP, &ex) == MB_OK && boot_proto_take_action(&bp) == BP_ACT_FORGET_APP);

    /* 8. 写 Flash 失败（假装校验失败）：UPG_STATE 4、LAST_ERR 0x0506 */
    boot_proto_program_done(&bp, AF_ERR_VERIFY);
    CHECK(bp.upg_state == BP_UPG_FAILED && bp.last_err == (BP_ERR_UPGRADE | AF_ERR_VERIFY));

    /* 9. 跳包：0x0303，再从头发一次就好了（新文件头清掉 LAST_ERR） */
    {
        uint8_t hdr[FILE_HDR_SIZE] = { 0 };
        put32(&hdr[0], 9); put32(&hdr[4], sizeof(img)); put32(&hdr[8], APP); put32(&hdr[12], crc);
        CHECK(send_rec(FILE_NO_CENTRAL_FW, 0, hdr, FILE_HDR_SIZE, &ex) == MB_OK && bp.upg_state == BP_UPG_RECEIVING && bp.last_err == 0);
        CHECK(send_rec(FILE_NO_CENTRAL_FW, 2, img, FILE_REC_DATA, &ex) == MB_ERR_EXCEPTION);
        CHECK(bp.last_err == (BP_ERR_FILE | FRX_ERR_SEQUENCE));
        CHECK(send_fw(FILE_NO_CENTRAL_FW, APP, crc, 9, &ex) == MB_OK && bp.upg_state == BP_UPG_PROGRAMMING);
        run_program();
        CHECK(bp.upg_state == BP_UPG_DONE && bp.app_ver == 9);
    }

    printf("boot_proto: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
