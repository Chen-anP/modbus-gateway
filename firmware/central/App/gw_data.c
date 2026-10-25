/*
 * gw_data.c - 中控的共享数据，说明见 gw_data.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "FreeRTOS.h"
#include "semphr.h"
#include "log.h"
#include "version.h"
#include "default_points.h"
#include "poll_task.h"
#include "gw_data.h"

static point_table_t     s_pt;
static reg_space_t       s_rs;
static sys_regs_t        s_sys;
static SemaphoreHandle_t s_mutex;
static uint8_t           s_inited;

void gw_lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void gw_unlock(void) { xSemaphoreGive(s_mutex); }

/* 系统区 0xF020 起的通道统计：从轮询任务取（读别的任务的 32 位计数，不加锁，见第 4 步笔记） */
static void get_ch_stat(uint8_t ch, sys_ch_stat_t *out)
{
    const point_sync_t *ps = poll_get((uint8_t)(ch - 1u));

    if (ps == NULL)
        return;
    out->ok         = ps->mb->st.ok;
    out->timeouts   = ps->mb->st.timeouts;
    out->crc_errors = ps->mb->st.crc_errors;
    out->offline    = point_sync_offline(ps);
}

void gw_data_init(void)
{
    static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE];
    uint32_t len;
    uint16_t bad = 0;
    pt_err_t e;

    if (s_inited)
        return;
    s_inited = 1;

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    sys_regs_init(&s_sys, FW_VERSION, get_ch_stat);
    reg_space_init(&s_rs, &s_sys, NULL, NULL);    /* 先不加锁：调度器还没启动，只有一个执行流 */

    len = default_points(buf, sizeof(buf));
    e = point_table_parse(&s_pt, buf, len, &bad);
    if (e != PT_OK) {                       /* 默认表写错了：不映射任何点，LAST_ERR 记下原因 */
        s_pt.n = 0;
        s_pt.nblocks = 0;
        s_sys.last_err = (uint16_t)(0x0100u | (unsigned)e);
        LOGE("gw", "default point table error: %s at point %u", pt_err_name(e), (unsigned)bad);
    }
    reg_space_load(&s_rs, &s_pt);
    s_rs.lock = gw_lock;                    /* 之后多个任务会同时用，装上锁 */
    s_rs.unlock = gw_unlock;
    LOGI("gw", "point table: %u points, %u read blocks", (unsigned)s_pt.n, (unsigned)s_pt.nblocks);
}

const point_table_t *gw_points(void) { return &s_pt; }
reg_space_t         *gw_rs(void)     { return &s_rs; }
sys_regs_t          *gw_sys(void)    { return &s_sys; }
