/*
 * test_poll.c - 主站轮询：周期、掉线判定、掉线后降频、恢复、异常码算在线
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I tests firmware/central/App/mb_rtu.c
 *       firmware/central/App/mb_master.c firmware/central/App/dev_health.c firmware/central/App/poll.c tests/test_poll.c
 *
 * 用 fake_bus.h 的模拟从机，假时间。run() 模拟 poll_task 的主循环：扫一遍、睡 50 ms。
 * 加参数 -v 运行可以看到 INFO 以上的日志：tests/out/test_poll.exe -v
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "fake_bus.h"
#include "poll.h"

static const poll_dev_cfg_t k_devs[] = {
    { "switch", 1, MB_TAB_DISCRETE, 0, 3, 200  },
    { "env",    2, MB_TAB_INPUT,    0, 2, 500  },
    { "temp",   3, MB_TAB_INPUT,    0, 2, 2000 },
};

static mb_master_t m;
static poll_ch_t ch;
static poll_dev_t devs[3];
static fb_slave_t *s1, *s2, *s3;

/* 跑 ms 毫秒：和 poll_task 一样，扫一遍、睡 50 ms */
static void run(uint32_t ms)
{
    uint32_t end = g_now + ms;
    while ((int32_t)(g_now - end) < 0) {
        poll_ch_step(&ch);
        g_now += 50;
    }
}

static void setup(void)
{
    fb_reset();
    g_now = 0;
    s1 = fb_add_slave(1); s1->bits[0] = 1; s1->bits[2] = 1;
    s2 = fb_add_slave(2); s2->regs[0] = 4095; s2->regs[1] = 1200;
    s3 = fb_add_slave(3); s3->regs[0] = 266;  s3->regs[1] = 455;
    mb_master_init(&m, &g_io, 100, 2);
    m.echo = MB_ECHO_NONE;
    CHECK(poll_ch_init(&ch, &m, devs, k_devs, 3) == 0);
}

int main(int argc, char **argv)
{
    g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    /* 1. 配置检查：个数 0 或超过 POLL_MAX_VALUES 拒绝 */
    {
        poll_dev_cfg_t bad[1] = { { "x", 9, MB_TAB_INPUT, 0, POLL_MAX_VALUES + 1, 100 } };
        poll_dev_t d[1];
        poll_ch_t c;
        fb_reset();
        mb_master_init(&m, &g_io, 100, 2);
        CHECK(poll_ch_init(&c, &m, d, bad, 1) == -1);
        bad[0].count = 0;
        CHECK(poll_ch_init(&c, &m, d, bad, 1) == -1);
    }

    /* 2. 上电第一遍：三个都读一次，都变成 online，值对 */
    setup();
    CHECK(poll_ch_step(&ch) == 3);
    CHECK(devs[0].health.state == DEV_ONLINE && devs[1].health.state == DEV_ONLINE && devs[2].health.state == DEV_ONLINE);
    CHECK(devs[0].values[0] == 1 && devs[0].values[1] == 0 && devs[0].values[2] == 1);
    CHECK(devs[1].values[0] == 4095 && devs[2].values[0] == 266 && devs[2].values[1] == 455);
    CHECK(poll_ch_find(&ch, 3) == &devs[2] && poll_ch_find(&ch, 9) == NULL);

    /* 3. 各按各的周期：10 s 内 switch 约 50 次、env 约 20 次、temp 约 5 次 */
    s1->requests = s2->requests = s3->requests = 0;
    run(10000);
    CHECK(s1->requests >= 45 && s1->requests <= 51);
    CHECK(s2->requests >= 19 && s2->requests <= 21);
    CHECK(s3->requests >= 4  && s3->requests <= 6);

    /* 4. 值变化能读到 */
    s1->bits[1] = 1;
    s3->regs[0] = 270;
    run(2100);
    CHECK(devs[0].values[1] == 1 && devs[2].values[0] == 270);

    /* 5. 拔掉 env：连续 3 次失败才掉线（每次 3 个尝试 × 100 ms） */
    s2->online = 0;
    run(500);                                       /* 失败 1 次 */
    CHECK(devs[1].health.state == DEV_ONLINE);
    run(1200);                                      /* 一共失败 3 次 */
    CHECK(devs[1].health.state == DEV_OFFLINE && devs[1].health.offline_count == 1);
    CHECK(devs[1].values[0] == 4095);               /* 掉线后保留最后一次的值（第 5 步点表会标"数据无效"） */

    /* 6. 掉线期间：每 2 s 才试一次（而不是每 500 ms），而且只试 1 次不重试（只等 100 ms，不是 300 ms） */
    {
        uint32_t fail0 = devs[1].health.fail_count, retry0 = m.st.retries;
        g_wait_ms = 0;
        run(10000);
        CHECK(devs[1].health.fail_count - fail0 >= 4 && devs[1].health.fail_count - fail0 <= 6);
        CHECK(m.st.retries == retry0);
        CHECK(g_wait_ms <= 6 * 100);
    }

    /* 7. 掉线设备不拖慢别人：这 10 s 里 switch 还是读了 45 次以上
     *    （如果每 500 ms 都等 300 ms 超时，switch 的 200 ms 周期就保不住了） */
    s1->requests = 0;
    run(10000);
    CHECK(s1->requests >= 45);

    /* 8. 插回来：最多 2 s 内恢复在线 */
    s2->online = 1;
    run(2100);
    CHECK(devs[1].health.state == DEV_ONLINE);

    /* 9. 从来没接过的设备：上电后 UNKNOWN → 3 次失败 → OFFLINE，不影响别的 */
    setup();
    s3->online = 0;
    run(10000);
    CHECK(devs[2].health.state == DEV_OFFLINE && devs[2].have_values == 0);
    CHECK(devs[0].health.state == DEV_ONLINE && devs[1].health.state == DEV_ONLINE);

    /* 10. 读不存在的地址 → 异常码：设备算在线（它回应了），不算失败 */
    {
        static const poll_dev_cfg_t ex_cfg[] = { { "sw-ir", 1, MB_TAB_INPUT, 6, 4, 200 } };   /* 6..9 超出 0..7 */
        poll_dev_t d[1];
        fb_reset(); g_now = 0;
        s1 = fb_add_slave(1);
        mb_master_init(&m, &g_io, 100, 2);
        m.echo = MB_ECHO_NONE;
        poll_ch_init(&ch, &m, d, ex_cfg, 1);
        g_log_warn = 0;
        run(2000);
        CHECK(d[0].health.state == DEV_ONLINE && d[0].last_rc == MB_ERR_EXCEPTION && d[0].last_ex == MB_EX_ILLEGAL_ADDRESS);
        CHECK(d[0].have_values == 0 && m.st.exceptions >= 9);
        CHECK(g_log_warn == 1);                     /* 同一个异常码只报一次，不刷屏 */
    }

    /* 11. 汇总不崩（-v 时能看到输出） */
    setup();
    run(1000);
    poll_ch_report(&ch);

    /* 12. 时间回绕：从 0xFFFFF000 开始跑 10 s，跨过 0，周期照常 */
    setup();
    g_now = 0xFFFFF000u;
    poll_ch_init(&ch, &m, devs, k_devs, 3);
    s1->requests = 0;
    run(10000);
    CHECK(s1->requests >= 45);

    printf("poll: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
