/*
 * test_mb_master.c - 主机请求层：回显、分包、垃圾数据、超时、重试、CRC 错、异常码、广播
 *
 *   gcc -std=c99 -Wall -Wextra -I firmware/central/App -I tests firmware/central/App/mb_rtu.c
 *       firmware/central/App/mb_master.c tests/test_mb_master.c
 *
 * 1~7 是第 2 步 test_bus_probe.c 的用例（transact 搬到了 mb_master.c），8 以后是第 3 步新增。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "fake_bus.h"

int main(void)
{
    mb_master_t m;
    uint8_t req[16], ok[16], junk[] = { 0x55, 0xAA };
    uint16_t req_len = mb_build_read(req, 3, MB_TAB_INPUT, 0, 2);   /* 03 04 00 00 00 02 + CRC */
    const uint8_t body[] = { 0x03, 0x04, 0x04, 0x00, 0xFA, 0x01, 0xC2 };   /* 温度 250，湿度 450 */
    uint16_t c = mb_crc16(body, 7), v[8];
    uint8_t both[32];
    mb_ex_t ex;
    int rc;

    memcpy(ok, body, 7); ok[7] = (uint8_t)c; ok[8] = (uint8_t)(c >> 8);
    memcpy(both, req, req_len); memcpy(both + req_len, ok, 9);

    fb_reset();
    mb_master_init(&m, &g_io, 100, 0);              /* 前 7 个用例不重试，和第 2 步一样 */

#define RUN() (v[0] = v[1] = 0, mb_master_request(&m, req, req_len, v, 8, &ex))
    fb_script(1, ok, 9);
    CHECK(RUN() == MB_OK && v[0] == 250 && v[1] == 450);                 /* 1. 正常 */
    fb_script(2, req, (int)req_len, ok, 9);
    CHECK(RUN() == MB_OK && v[0] == 250);                                /* 2. 先回显再回应 */
    fb_script(1, both, (int)req_len + 9);
    CHECK(RUN() == MB_OK && v[1] == 450);                                /* 3. 回显和回应粘在一段 */
    fb_script(3, ok, 2, ok + 2, 3, ok + 5, 4);
    CHECK(RUN() == MB_OK && v[0] == 250);                                /* 4. 分三段，开头和请求一样 */
    fb_script(0);
    CHECK(RUN() == MB_ERR_TIMEOUT && m.st.timeouts == 1);                /* 5. 没有回应 */
    fb_script(1, ok, 5);
    CHECK(RUN() == MB_ERR_TIMEOUT && m.st.timeouts == 2);                /* 6. 只来了一半 */
    fb_script(2, junk, 2, ok, 9);
    rc = RUN();
    CHECK(rc != MB_OK && rc != MB_ERR_EXCEPTION);                        /* 7. 前面有垃圾：报错，不崩溃 */
    CHECK(m.st.requests == 7 && m.st.ok == 4 && m.st.retries == 0);

    /* ---- 第 3 步：接模拟从机 ---- */
    fb_reset();
    fb_slave_t *s3 = fb_add_slave(3);
    s3->regs[0] = 266; s3->regs[1] = 455;
    mb_master_init(&m, &g_io, 100, 2);              /* 正式参数：超时 100 ms，重试 2 次 */

    /* 8. 正常读 */
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_OK && v[0] == 266 && v[1] == 455);
    CHECK(g_sends == 1 && m.st.retries == 0);

    /* 9. 第一次回应 CRC 坏了，重试一次就好 */
    s3->corrupt_next = 1;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_OK);
    CHECK(m.st.crc_errors == 1 && m.st.retries == 1 && g_sends == 3);

    /* 10. 连续 3 次 CRC 坏：重试用完，返回 CRC 错 */
    s3->corrupt_next = 3;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_ERR_CRC && m.st.crc_errors == 4);

    /* 11. 设备拔掉：1 + 2 次全超时，一共等了 3 × 100 ms */
    s3->online = 0;
    g_wait_ms = 0;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_ERR_TIMEOUT);
    CHECK(g_wait_ms == 300);
    s3->online = 1;

    /* 12. 异常码：读不存在的地址 → 异常 02，不重试（从机是活的） */
    uint32_t sends = g_sends;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 6, 4, v, 8, &ex) == MB_ERR_EXCEPTION && ex == MB_EX_ILLEGAL_ADDRESS);
    CHECK(g_sends == sends + 1 && m.st.exceptions == 1);

    /* 13. 有回显的收发器：照样能读 */
    g_echo = 1;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_OK && v[0] == 266);
    g_echo = 0;

    /* 14. 写单个寄存器：06 的回应和请求一模一样。
     *     AUTO 模式下只收到一份，要等满超时才敢当成回应（这是第 3 步写测试时发现的 bug：
     *     第 2 步的回显检查会把这种回应吃掉，返回超时） */
    {
        uint32_t t0 = g_now;
        CHECK(mb_master_write_single(&m, 3, MB_TAB_HOLDING, 1, 0x1234, &ex) == MB_OK && s3->regs[1] == 0x1234);
        CHECK(g_now - t0 >= 100);
    }
    /* 14b. NONE 模式（中控板实测不回显）：马上就成功 */
    {
        uint32_t t0 = g_now;
        m.echo = MB_ECHO_NONE;
        CHECK(mb_master_write_single(&m, 3, MB_TAB_HOLDING, 1, 0x4321, &ex) == MB_OK && s3->regs[1] == 0x4321);
        CHECK(g_now - t0 < 10);
        CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_OK && v[0] == 266);
    }
    /* 14c. ALWAYS 模式 + 回显的收发器：回显一份、回应一份，马上成功 */
    {
        uint32_t t0 = g_now;
        m.echo = MB_ECHO_ALWAYS; g_echo = 1;
        CHECK(mb_master_write_single(&m, 3, MB_TAB_HOLDING, 1, 0x5555, &ex) == MB_OK && s3->regs[1] == 0x5555);
        CHECK(g_now - t0 < 10);
        /* 从机拔掉：只收到回显，不能当成功 */
        s3->online = 0;
        CHECK(mb_master_write_single(&m, 3, MB_TAB_HOLDING, 1, 0x6666, &ex) == MB_ERR_TIMEOUT);
        s3->online = 1; g_echo = 0; m.echo = MB_ECHO_AUTO;
    }

    /* 15. 广播写：从机执行但不回应，不等超时、不重试 */
    sends = g_sends; g_wait_ms = 0;
    CHECK(mb_master_write_single(&m, 0, MB_TAB_HOLDING, 0, 77, &ex) == MB_OK);
    CHECK(s3->regs[0] == 77 && g_sends == sends + 1 && g_wait_ms == 0);

    /* 16. 发送失败（DMA 超时）：计 send_errors，会重试 */
    g_send_fail = 1;
    s3->regs[0] = 266;
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex) == MB_OK && m.st.send_errors == 1);

    /* 17. 参数非法：个数超过结果数组 */
    CHECK(mb_master_read(&m, 3, MB_TAB_INPUT, 0, 4, v, 2, &ex) == MB_ERR_PARAM);

    /* 18. 帧间隔：刚收完回应马上发下一帧，要先等够 2 ms */
    {
        uint32_t t0;
        mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex);
        t0 = g_now;
        mb_master_read(&m, 3, MB_TAB_INPUT, 0, 2, v, 8, &ex);
        CHECK(g_now - t0 >= 2 + 1);                 /* 等 2 ms + 发送 1 ms（假时间） */
    }

    printf("mb_master: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
