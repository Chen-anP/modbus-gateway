/*
 * test_sys_regs.c - 系统区寄存器：用真实的请求帧走一遍 mb_slave_handle()，再用 mb_parse_rsp() 解析回应
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c
 *       firmware/central/App/sys_regs.c tests/test_sys_regs.c
 *
 * 这样测的是"电脑发过来的字节 → 中控回的字节"整条路径，和 Modbus Poll 看到的一样。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "mb_rtu.h"
#include "sys_regs.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* 假的通道统计：ch1 有一些计数，ch2 有一个掉线设备；ch1 的 ok 故意超过 65535 */
static void fake_stat(uint8_t ch, sys_ch_stat_t *o)
{
    if (ch == 1) { o->ok = 70000; o->timeouts = 12; o->crc_errors = 3; o->offline = 0; }
    else         { o->ok = 5;     o->timeouts = 29; o->crc_errors = 0; o->offline = 1; }
}

static sys_regs_t s;
static mb_slave_ops_t ops;

/* 发一个请求，返回 mb_parse_rsp 的结果；*replied = 中控有没有回 */
static int ask(const uint8_t *req, uint16_t len, uint16_t *v, uint16_t max, mb_ex_t *ex, int *replied)
{
    uint8_t rsp[MB_ADU_MAX];
    uint16_t n = mb_slave_handle(1, &ops, req, len, rsp);

    *replied = (n > 0);
    if (n == 0)
        return 99;
    return mb_parse_rsp(req, len, rsp, n, v, max, ex);
}

int main(void)
{
    uint8_t req[MB_ADU_MAX];
    uint16_t v[16], len;
    mb_ex_t ex;
    int rep;

    sys_regs_init(&s, 0x0004, fake_stat);
    sys_regs_ops(&s, &ops);

    /* 1. 读 0xF000～0xF004：CMD 读出 0，STATE 0，FW_VER 4，点数 0，错误 0 */
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF000, 5);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_OK);
    CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0x0004 && v[3] == 0 && v[4] == 0);

    /* 2. 读两个通道的统计 0xF020～0xF027；70000 取低 16 位 = 4464 */
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF020, 8);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_OK);
    CHECK(v[0] == (uint16_t)70000u && v[1] == 12 && v[2] == 3 && v[3] == 0);
    CHECK(v[4] == 5 && v[5] == 29 && v[6] == 0 && v[7] == 1);

    /* 3. 没定义的地址：0xF006、跨过空洞的 0xF005～0xF006、0xF028、映射区 0 → 异常 02 */
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF006, 1);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF005, 2);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF027, 2);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0, 1);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 4. 其他三张表都没有：读 3x / 0x / 1x 的 0xF000 → 异常 02 */
    len = mb_build_read(req, 1, MB_TAB_INPUT, 0xF000, 1);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    len = mb_build_read(req, 1, MB_TAB_COIL, 0xF000, 1);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    len = mb_build_read(req, 1, MB_TAB_DISCRETE, 0xF000, 1);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);

    /* 5. 写 CMD = 1（重启）：正常回应（原样回显），命令被记下，取一次后清零 */
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, SYS_CMD_REBOOT);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_OK);
    CHECK(sys_regs_take_cmd(&s) == SYS_CMD_REBOOT && sys_regs_take_cmd(&s) == SYS_CMD_NONE);

    /* 6. 写不支持的命令 0x0C00、0x0003、0xFFFF → 异常 03，什么都不记 */
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, 0x0C00);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_VALUE);
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, 0x0003);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_VALUE);
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, 0xFFFF);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_VALUE);
    CHECK(sys_regs_take_cmd(&s) == SYS_CMD_NONE);

    /* 6b. 第 7a 步的三个新命令：都接受，记下来等 up 任务执行 */
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, SYS_CMD_CLEAR_TABLE);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_OK && sys_regs_take_cmd(&s) == SYS_CMD_CLEAR_TABLE);
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, SYS_CMD_ENTER_BOOT);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_OK && sys_regs_take_cmd(&s) == SYS_CMD_ENTER_BOOT);
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_CMD, SYS_CMD_CLEAR_STAY);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_OK && sys_regs_take_cmd(&s) == SYS_CMD_CLEAR_STAY);

    /* 6c. PT_SOURCE（0xF005）：读得到，不能写 */
    s.pt_source = SYS_PT_FLASH;
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF004, 2);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_OK && v[1] == SYS_PT_FLASH);
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_PT_SOURCE, 0);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(s.pt_source == SYS_PT_FLASH);

    /* 7. 写只读寄存器 FW_VER → 异常 02，版本号不变 */
    len = mb_build_write_single(req, 1, MB_TAB_HOLDING, SYS_REG_FW_VER, 0x9999);
    CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(s.fw_ver == 0x0004);

    /* 8. 一次写 0xF000～0xF001（CMD = 1、STATE = 0）：STATE 只读，整条拒绝，CMD 也不能被写进去（先检查后写） */
    {
        const uint16_t vals[2] = { SYS_CMD_REBOOT, 0 };
        len = mb_build_write_regs(req, 1, SYS_REG_CMD, 2, vals);
        CHECK(ask(req, len, NULL, 0, &ex, &rep) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
        CHECK(sys_regs_take_cmd(&s) == SYS_CMD_NONE);
    }

    /* 9. 地址不是 1：不回应 */
    len = mb_build_read(req, 2, MB_TAB_HOLDING, 0xF000, 1);
    ask(req, len, v, 16, &ex, &rep);
    CHECK(rep == 0);

    /* 10. CRC 错：不回应 */
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF000, 1);
    req[len - 1] ^= 0xFF;
    ask(req, len, v, 16, &ex, &rep);
    CHECK(rep == 0);

    /* 11. 广播写 CMD = 1：不回应，但命令照样执行 */
    len = mb_build_write_single(req, 0, MB_TAB_HOLDING, SYS_REG_CMD, SYS_CMD_REBOOT);
    ask(req, len, NULL, 0, &ex, &rep);
    CHECK(rep == 0 && sys_regs_take_cmd(&s) == SYS_CMD_REBOOT);

    /* 12. 没有统计来源（NULL）：统计读出来是 0，不崩 */
    sys_regs_init(&s, 0x0004, NULL);
    sys_regs_ops(&s, &ops);
    len = mb_build_read(req, 1, MB_TAB_HOLDING, 0xF020, 8);
    CHECK(ask(req, len, v, 16, &ex, &rep) == MB_OK && v[0] == 0 && v[7] == 0);

    printf("sys_regs: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
