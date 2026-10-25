/*
 * up_link.c - 上行任务：从 USB 收一帧 Modbus 请求 → mb_slave_handle() → 回一帧
 *
 * 一帧怎么算收完：USB 虚拟串口没有"字符间隔"（数据按 USB 包到达），所以和下行一样用帧组装器：
 * 按功能码和长度字段算出整帧多长，收够就是一帧；两个字节间隔超过 50 ms 丢弃半帧
 * （USB 包之间的间隔比 RS485 的字节间隔大得多，所以比下行的 5 ms 宽松）。
 *
 * 第 4 步只有系统区（sys_regs.c）；第 5 步加上点表映射区。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"           /* NVIC_SystemReset */
#include "log.h"
#include "mb_rtu.h"
#include "port_usb.h"
#include "poll_task.h"
#include "sys_regs.h"
#include "version.h"
#include "up_link.h"

#define UP_GAP_MS           50u
#define UP_WRITE_TIMEOUT_MS 100u
#define UP_REPORT_MS        10000u
#define UP_STACK_WORDS      512u                    /* 2 KB：帧组装器约 270 B + 回应缓冲 256 B + 日志 */
#define UP_PRIORITY         (tskIDLE_PRIORITY + 4)  /* 比轮询任务高：上位机的请求要及时回应 */

static sys_regs_t     s_sys;
static mb_slave_ops_t s_ops;
static uint32_t       s_frames, s_replies, s_exceptions, s_write_fail;

/* 系统区要的通道统计：从轮询模块取。
 * 这里是另一个任务在读 ch 任务的计数：32 位对齐的读写在 M33 上是一条指令，不会读到"半个数"；
 * 几个数之间可能差一次轮询，统计用途可以接受。第 5 步共享点表数据时再用互斥锁 */
static void get_ch_stat(uint8_t ch, sys_ch_stat_t *out)
{
    const poll_ch_t *p = poll_get((uint8_t)(ch - 1u));

    if (p == NULL)
        return;
    out->ok         = p->mb->st.ok;
    out->timeouts   = p->mb->st.timeouts;
    out->crc_errors = p->mb->st.crc_errors;
    out->offline    = 0;
    for (uint8_t i = 0; i < p->ndev; i++)
        if (p->devs[i].health.state == DEV_OFFLINE)
            out->offline++;
}

/* 处理一帧完整的请求 */
static void handle_frame(const uint8_t *req, uint16_t len)
{
    uint8_t rsp[MB_ADU_MAX];
    uint16_t rsp_len;

    s_frames++;
    log_hex(LOG_LVL_DEBUG, "up", "rx", req, len);
    rsp_len = mb_slave_handle(UP_SLAVE_ADDR, &s_ops, req, len, rsp);
    if (rsp_len == 0)
        return;                                     /* CRC 错、不是发给我的、广播：不回应 */
    if (rsp[1] & 0x80u) {
        s_exceptions++;
        LOGD("up", "exception %u", (unsigned)rsp[2]);
    }
    log_hex(LOG_LVL_DEBUG, "up", "tx", rsp, rsp_len);
    if (port_usb_write(rsp, rsp_len, UP_WRITE_TIMEOUT_MS) != 0) {
        s_write_fail++;
        LOGW("up", "write timeout (PC not reading?)");
        return;
    }
    s_replies++;
}

/* 回应发出去以后再执行命令 */
static void run_pending_cmd(void)
{
    if (sys_regs_take_cmd(&s_sys) == SYS_CMD_REBOOT) {
        LOGW("up", "reboot by command");
        vTaskDelay(pdMS_TO_TICKS(100));             /* 等 USB 把回应送到电脑、日志打完 */
        NVIC_SystemReset();
    }
}

static void up_link_task(void *arg)
{
    mb_framer_t f;
    uint8_t chunk[64];
    uint32_t next_report = 0, last_frames = 0;
    (void)arg;

    mb_framer_init(&f, 1, UP_GAP_MS);               /* 1 = 按"请求帧"格式算长度 */
    LOGI("up", "slave addr %u on USB CDC, fw 0x%04X", (unsigned)UP_SLAVE_ADDR, (unsigned)FW_VERSION);

    for (;;) {
        uint16_t n = port_usb_read(chunk, sizeof(chunk), 1000);
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint8_t *d = chunk;

        while (n > 0) {
            uint16_t used;
            if (mb_framer_feed(&f, d, n, now, &used)) {
                handle_frame(f.buf, f.len);
                mb_framer_reset(&f);
                run_pending_cmd();
            }
            d += used;
            n = (uint16_t)(n - used);
        }

        if ((int32_t)(now - next_report) >= 0) {    /* 每 10 秒，有新请求时打一行汇总 */
            next_report = now + UP_REPORT_MS;
            if (s_frames != last_frames) {
                const port_usb_stats_t *u = port_usb_stats();
                LOGI("up", "frames %lu replies %lu ex %lu wfail %lu | usb rx %lu tx %lu",
                     (unsigned long)s_frames, (unsigned long)s_replies, (unsigned long)s_exceptions,
                     (unsigned long)s_write_fail, (unsigned long)u->rx_bytes, (unsigned long)u->tx_bytes);
                last_frames = s_frames;
            }
        }
    }
}

void up_link_start(void)
{
    BaseType_t r;
    int usb_ok;

    sys_regs_init(&s_sys, FW_VERSION, get_ch_stat);
    sys_regs_ops(&s_sys, &s_ops);
    usb_ok = port_usb_init();                       /* 不能直接写在 configASSERT() 里：断言关掉时整句会被删掉 */
    configASSERT(usb_ok == 0);
    (void)usb_ok;
    r = xTaskCreate(up_link_task, "up", UP_STACK_WORDS, NULL, UP_PRIORITY, NULL);
    configASSERT(r == pdPASS);
}
