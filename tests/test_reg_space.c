/*
 * test_reg_space.c - 寄存器空间：映射区读写、待写入标记、下发结果、读回更新、系统区转交、锁的使用
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c firmware/central/App/sys_regs.c
 *       firmware/central/App/point_table.c firmware/central/App/default_points.c
 *       firmware/central/App/reg_space.c tests/test_reg_space.c
 *
 * 上位机这一侧都用真实的请求帧走 mb_slave_handle()，和 Modbus Poll 看到的一样。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <stdio.h>
#include <string.h>
#include "mb_rtu.h"
#include "point_table.h"
#include "default_points.h"
#include "reg_space.h"

static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static point_table_t pt;
static sys_regs_t sys;
static reg_space_t rs;
static mb_slave_ops_t ops;
static int g_locked, g_lock_count, g_lock_error;

static void lk(void)  { if (g_locked) g_lock_error++; g_locked = 1; g_lock_count++; }
static void ulk(void) { if (!g_locked) g_lock_error++; g_locked = 0; }

/* 上位机发一帧：返回 mb_parse_rsp 的结果 */
static int up(const uint8_t *req, uint16_t len, uint16_t *v, uint16_t max, mb_ex_t *ex)
{
    uint8_t rsp[MB_ADU_MAX];
    uint16_t n = mb_slave_handle(1, &ops, req, len, rsp);
    return n ? mb_parse_rsp(req, len, rsp, n, v, max, ex) : 99;
}

static int up_read(mb_table_t tab, uint16_t addr, uint16_t count, uint16_t *v, mb_ex_t *ex)
{
    uint8_t req[16];
    uint16_t len = mb_build_read(req, 1, tab, addr, count);
    return up(req, len, v, 16, ex);
}

static int up_write(mb_table_t tab, uint16_t addr, uint16_t value, mb_ex_t *ex)
{
    uint8_t req[16];
    uint16_t len = mb_build_write_single(req, 1, tab, addr, value);
    return up(req, len, NULL, 0, ex);
}

int main(void)
{
    static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE];
    uint16_t v[16], val, len;
    mb_ex_t ex;
    int coil2, led_env, temp;

    len = (uint16_t)default_points(buf, sizeof(buf));
    CHECK(point_table_parse(&pt, buf, len, NULL) == PT_OK);
    sys_regs_init(&sys, 5, NULL);
    reg_space_init(&rs, &sys, lk, ulk);
    reg_space_load(&rs, &pt);
    reg_space_ops(&rs, &ops);
    coil2   = point_table_find(&pt, MB_TAB_COIL, 2);         /* 开关量板 LED1 */
    led_env = point_table_find(&pt, MB_TAB_COIL, 12);        /* 环境监测板 LED1 */
    temp    = point_table_find(&pt, MB_TAB_INPUT, 20);       /* 温度 */

    /* 1. 加载后 POINT_CNT = 22（系统区 0xF003）；锁成对使用 */
    CHECK(up_read(MB_TAB_HOLDING, 0xF003, 1, v, &ex) == MB_OK && v[0] == 22);
    CHECK(g_lock_count == 1 && g_lock_error == 0 && !g_locked);

    /* 2. 未同步的点读到 0；没映射的地址回 02；跨过空洞（0x 4～5：5 没映射）也回 02 */
    CHECK(up_read(MB_TAB_INPUT, 20, 2, v, &ex) == MB_OK && v[0] == 0 && v[1] == 0);
    CHECK(up_read(MB_TAB_INPUT, 22, 1, v, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(up_read(MB_TAB_COIL, 4, 2, v, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(up_read(MB_TAB_HOLDING, 0, 1, v, &ex) == MB_ERR_EXCEPTION);           /* 默认表没映射 4x */
    CHECK(up_read(MB_TAB_INPUT, 499, 2, v, &ex) == MB_ERR_EXCEPTION);           /* 超出 499 */

    /* 3. 通道任务读到值 → 上位机看到；update 返回"变了没有" */
    CHECK(reg_space_update(&rs, (uint16_t)temp, 266) == 1);
    CHECK(reg_space_update(&rs, (uint16_t)temp, 266) == 0);
    CHECK(up_read(MB_TAB_INPUT, 20, 1, v, &ex) == MB_OK && v[0] == 266);

    /* 4. 上电不误写：什么都没写时，没有待写入的点 */
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1 && reg_space_next_pending(&rs, 2, 0, &val) == -1);

    /* 5. 上位机写线圈 2 → 标记待写入，上位机马上读到自己写的值；只出现在 ch1 */
    CHECK(up_write(MB_TAB_COIL, 2, 1, &ex) == MB_OK);
    CHECK(up_read(MB_TAB_COIL, 2, 1, v, &ex) == MB_OK && v[0] == 1);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == coil2 && val == 1);
    CHECK(reg_space_next_pending(&rs, 2, 0, &val) == -1);
    CHECK(rs.writes == 1);

    /* 6. 待写入期间，读回来的旧值不能覆盖 */
    CHECK(reg_space_update(&rs, (uint16_t)coil2, 0) == 0);
    CHECK(up_read(MB_TAB_COIL, 2, 1, v, &ex) == MB_OK && v[0] == 1);

    /* 7. 下发超时 → 保留标记 */
    reg_space_write_result(&rs, (uint16_t)coil2, 1, RS_WRITE_RETRY);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == coil2);

    /* 8. 下发成功 → 清标记，值变成写下去的值；之后读回来的值正常更新 */
    reg_space_write_result(&rs, (uint16_t)coil2, 1, RS_WRITE_OK);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1);
    CHECK(reg_space_get(&rs, (uint16_t)coil2) == 1);
    CHECK(reg_space_update(&rs, (uint16_t)coil2, 0) == 1 && reg_space_get(&rs, (uint16_t)coil2) == 0);

    /* 9. 下发途中上位机又写了新值：旧值下发成功后，标记要保留，下一轮发新值 */
    up_write(MB_TAB_COIL, 2, 1, &ex);
    reg_space_next_pending(&rs, 1, 0, &val);                    /* 通道任务取到 1，准备发 */
    up_write(MB_TAB_COIL, 2, 0, &ex);                           /* 还没发完，上位机改成 0 */
    reg_space_write_result(&rs, (uint16_t)coil2, val, RS_WRITE_OK);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == coil2 && val == 0);
    reg_space_write_result(&rs, (uint16_t)coil2, 0, RS_WRITE_OK);

    /* 10. 设备拒绝 → 放弃，LAST_ERR = 0x0200 */
    up_write(MB_TAB_COIL, 12, 1, &ex);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == led_env);
    reg_space_write_result(&rs, (uint16_t)led_env, 1, RS_WRITE_REJECTED);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1);
    CHECK(up_read(MB_TAB_HOLDING, 0xF004, 1, v, &ex) == MB_OK && v[0] == 0x0200);

    /* 11. 多个待写入：from 参数能一个个往后找 */
    up_write(MB_TAB_COIL, 0, 1, &ex);
    up_write(MB_TAB_COIL, 14, 1, &ex);
    {
        int a = reg_space_next_pending(&rs, 1, 0, &val);
        int b = reg_space_next_pending(&rs, 1, (uint16_t)(a + 1), &val);
        CHECK(a >= 0 && b > a && reg_space_next_pending(&rs, 1, (uint16_t)(b + 1), &val) == -1);
    }

    /* 12. 一次写多个线圈（0F）：先检查后写——范围里有没映射的地址，整条拒绝，一个都不标记 */
    {
        uint8_t req[32] = { 1, MB_FC_WRITE_COILS, 0x00, 0x03, 0x00, 0x03, 0x01, 0x07 };  /* 线圈 3～5 = 1 */
        uint16_t crc = mb_crc16(req, 8), before = (uint16_t)rs.writes;
        req[8] = (uint8_t)crc; req[9] = (uint8_t)(crc >> 8);
        CHECK(up(req, 10, NULL, 0, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
        CHECK(rs.writes == before);
    }

    /* 13. 系统区仍然可用：版本、写重启命令 */
    CHECK(up_read(MB_TAB_HOLDING, 0xF002, 1, v, &ex) == MB_OK && v[0] == 5);
    CHECK(up_write(MB_TAB_HOLDING, 0xF000, 1, &ex) == MB_OK && sys_regs_take_cmd(&sys) == SYS_CMD_REBOOT);

    /* 14. 换表：所有状态清零 */
    reg_space_load(&rs, &pt);
    CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1 && reg_space_get(&rs, (uint16_t)temp) == 0);

    /* 15. 锁：每次加锁都解了，没有重复加锁（回调里不加锁，否则 up 任务会自己锁死自己） */
    CHECK(g_lock_error == 0 && !g_locked);

    printf("reg_space: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
