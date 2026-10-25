/*
 * test_point_sync.c - 点同步：用默认点表和模拟的三块传感器，走"上位机写 → 下发 → 读回"的完整过程
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I tests firmware/central/App/mb_rtu.c
 *       firmware/central/App/mb_master.c firmware/central/App/dev_health.c firmware/central/App/sys_regs.c
 *       firmware/central/App/point_table.c firmware/central/App/default_points.c
 *       firmware/central/App/reg_space.c firmware/central/App/point_sync.c tests/test_point_sync.c
 *
 * 加 -v 运行能看到 INFO 以上的日志。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "fake_bus.h"
#include "point_table.h"
#include "default_points.h"
#include "reg_space.h"
#include "point_sync.h"

static point_table_t pt;
static sys_regs_t sys;
static reg_space_t rs;
static mb_slave_ops_t ops;
static mb_master_t m1, m2;
static point_sync_t ps1, ps2;
static fb_slave_t *sw, *env, *th;

/* 两个通道共用一条假总线（三个从机地址不同，互不干扰），轮流跑一轮，每轮之后睡 50 ms */
static void run(uint32_t ms)
{
    uint32_t end = g_now + ms;
    while ((int32_t)(g_now - end) < 0) {
        point_sync_step(&ps1);
        point_sync_step(&ps2);
        g_now += 50;
    }
}

static uint16_t up_read1(mb_table_t tab, uint16_t addr)
{
    uint8_t req[16], rsp[MB_ADU_MAX];
    uint16_t v[1] = { 0xFFFF }, len = mb_build_read(req, 1, tab, addr, 1), n;
    mb_ex_t ex;
    n = mb_slave_handle(1, &ops, req, len, rsp);
    if (n == 0 || mb_parse_rsp(req, len, rsp, n, v, 1, &ex) != MB_OK)
        return 0xEEEE;
    return v[0];
}

static int up_write1(mb_table_t tab, uint16_t addr, uint16_t value)
{
    uint8_t req[16], rsp[MB_ADU_MAX];
    uint16_t len = mb_build_write_single(req, 1, tab, addr, value), n;
    mb_ex_t ex;
    n = mb_slave_handle(1, &ops, req, len, rsp);
    return n ? mb_parse_rsp(req, len, rsp, n, NULL, 0, &ex) : 99;
}

static void setup(void)
{
    static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE];
    uint32_t len;

    fb_reset();
    g_now = 0;
    sw  = fb_add_slave(1); sw->disc[0] = 1; sw->disc[2] = 1; sw->coil[3] = 1;   /* 开关量：按键 1、3 按下；LED2 本来就亮 */
    env = fb_add_slave(2); env->input[0] = 3700; env->input[1] = 4095;
    th  = fb_add_slave(3); th->input[0] = 266; th->input[1] = 455;
    len = default_points(buf, sizeof(buf));
    point_table_parse(&pt, buf, len, NULL);
    sys_regs_init(&sys, 5, NULL);
    reg_space_init(&rs, &sys, NULL, NULL);
    reg_space_load(&rs, &pt);
    reg_space_ops(&rs, &ops);
    mb_master_init(&m1, &g_io, 100, 2); m1.echo = MB_ECHO_NONE;
    mb_master_init(&m2, &g_io, 100, 2); m2.echo = MB_ECHO_NONE;
    point_sync_init(&ps1, 1, &m1, &rs, &pt);
    point_sync_init(&ps2, 2, &m2, &rs, &pt);
}

int main(int argc, char **argv)
{
    uint32_t sends, w;

    g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    setup();

    /* 1. 从点表里找出每个通道的设备 */
    CHECK(ps1.ndev == 2 && ps2.ndev == 1 && ps2.devs[0].addr == 3);

    /* 2. 一轮之后，上位机能读到三块板的数据；ch1 一轮 4 次读请求（4 个读块），ch2 2 次 */
    sends = g_sends;
    point_sync_step(&ps1);
    CHECK(g_sends - sends == 4);
    point_sync_step(&ps2);
    CHECK(g_sends - sends == 6);
    CHECK(up_read1(MB_TAB_DISCRETE, 0) == 1 && up_read1(MB_TAB_DISCRETE, 1) == 0 && up_read1(MB_TAB_DISCRETE, 2) == 1);
    CHECK(up_read1(MB_TAB_INPUT, 10) == 3700 && up_read1(MB_TAB_INPUT, 11) == 4095);
    CHECK(up_read1(MB_TAB_INPUT, 20) == 266 && up_read1(MB_TAB_INPUT, 21) == 455);
    CHECK(up_read1(MB_TAB_COIL, 3) == 1);                       /* 线圈也读回来了 */

    /* 3. 上电不误写：跑 2 秒，三块板一次写请求都没收到（LED2 还亮着） */
    run(2000);
    CHECK(sw->writes == 0 && env->writes == 0 && th->writes == 0 && sw->coil[3] == 1);

    /* 4. 传感器的值变了，下一轮上位机就能读到 */
    th->input[0] = 270;
    sw->disc[1] = 1;
    run(100);
    CHECK(up_read1(MB_TAB_INPUT, 20) == 270 && up_read1(MB_TAB_DISCRETE, 1) == 1);

    /* 5. 上位机写线圈 2（开关量板 LED1）→ 下一轮写到设备 → 读回来也是 1，待写入清除 */
    CHECK(up_write1(MB_TAB_COIL, 2, 1) == MB_OK);
    w = sw->writes;
    run(50);
    CHECK(sw->writes == w + 1 && sw->coil[2] == 1);
    CHECK(up_read1(MB_TAB_COIL, 2) == 1 && ps1.writes_ok == 1);
    {
        uint16_t val;
        CHECK(reg_space_next_pending(&rs, 1, 0, &val) == -1);
    }
    run(500);
    CHECK(sw->writes == w + 1);                                 /* 只写一次，不会反复写 */

    /* 6. 写 ch2 的点（温湿度板 LED3 = 0x 24）走 ch2 */
    CHECK(up_write1(MB_TAB_COIL, 24, 1) == MB_OK);
    run(50);
    CHECK(th->coil[4] == 1 && ps2.writes_ok == 1);

    /* 7. 拔掉环境监测：约 1～2 秒掉线；开关量照常更新；掉线后每 2 秒只试 1 次 */
    env->online = 0;
    run(2000);
    CHECK(point_sync_offline(&ps1) == 1);
    sw->disc[0] = 0;
    run(200);
    CHECK(up_read1(MB_TAB_DISCRETE, 0) == 0);
    {
        uint32_t f0 = ps1.devs[1].health.fail_count;
        run(10000);
        CHECK(ps1.devs[1].health.fail_count - f0 >= 4 && ps1.devs[1].health.fail_count - f0 <= 6);
    }

    /* 8. 掉线期间上位机写它的点：保留待写入，不下发；它回来后马上下发 */
    CHECK(up_write1(MB_TAB_COIL, 12, 1) == MB_OK);              /* 环境监测板 LED1 */
    run(500);
    CHECK(env->coil[2] == 0);
    {
        uint16_t val;
        CHECK(reg_space_next_pending(&rs, 1, 0, &val) >= 0);
    }
    env->online = 1;
    run(2500);
    CHECK(point_sync_offline(&ps1) == 0 && env->coil[2] == 1);
    CHECK(up_read1(MB_TAB_COIL, 12) == 1);

    /* 9. 设备拒绝写入（地址超出它的范围）→ 放弃、记 LAST_ERR、不会反复重试 */
    {
        static uint8_t buf[16];
        const pt_point_t bad[] = { { MB_TAB_COIL, 1, 1, 7, 9 } };   /* 中控 0x 7 → 开关量板 0x 9（模拟从机只有 0～7） */
        static point_table_t t2;
        uint32_t len = point_table_encode(bad, 1, buf, sizeof(buf));
        point_table_parse(&t2, buf, len, NULL);
        reg_space_load(&rs, &t2);
        point_sync_init(&ps1, 1, &m1, &rs, &t2);
        g_log_warn = 0;
        CHECK(up_write1(MB_TAB_COIL, 7, 1) == MB_OK);
        point_sync_step(&ps1);
        CHECK(ps1.writes_rejected == 1 && sys.last_err == 0x0200);
        point_sync_step(&ps1);
        point_sync_step(&ps1);
        CHECK(ps1.writes_rejected == 1);                         /* 只试了一次 */
        /* 读块也回异常：设备在线（回应了），WARN 只报一次 */
        CHECK(point_sync_offline(&ps1) == 0);
        CHECK(g_log_warn == 2);                                  /* 写被拒 1 次 + 读块异常 1 次 */
    }

    /* 10. 汇总不崩 */
    setup();
    run(1000);
    point_sync_report(&ps1);
    point_sync_report(&ps2);

    printf("point_sync: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
