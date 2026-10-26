/*
 * gw_data.c - 中控的共享数据，说明见 gw_data.h
 *
 * 换点表的过程（gw_apply_table，在 up 任务里，回应已经发出去了）：
 *   1. 拿 ch1、ch2 两把轮锁：等两个通道各自跑完当前这一轮（最长约 1 秒：设备超时重试的情况）
 *   2. 把临时表复制成正在用的表，reg_space_load()：所有点回到"未同步"，待写入清掉
 *   3. 点表版本号 + 1，放开轮锁
 *   4. 通道任务下一轮开始时发现版本号变了，point_sync_init() 按新表重新找设备
 * 拿锁的顺序固定（先 ch1 后 ch2），up 任务在拿轮锁时不持有 gw_lock——避免互相等待（死锁）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "log.h"
#include "version.h"
#include "default_points.h"
#include "table_loader.h"
#include "poll_task.h"
#include "gw_data.h"

static point_table_t     s_pt;
static reg_space_t       s_rs;
static sys_regs_t        s_sys;
static table_loader_t    s_loader;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_round[PT_CH_COUNT];
static volatile uint32_t s_table_gen;           /* 点表版本号，每换一次 + 1 */
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
    s_round[0] = xSemaphoreCreateMutex();
    s_round[1] = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL && s_round[0] != NULL && s_round[1] != NULL);
    sys_regs_init(&s_sys, FW_VERSION, get_ch_stat);
    reg_space_init(&s_rs, &s_sys, NULL, NULL);    /* 先不加锁：调度器还没启动，只有一个执行流 */
    table_loader_init(&s_loader);

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
    s_table_gen = 1;
    LOGI("gw", "point table: %u points, %u read blocks (built-in)", (unsigned)s_pt.n, (unsigned)s_pt.nblocks);
}

const point_table_t *gw_points(void) { return &s_pt; }
reg_space_t         *gw_rs(void)     { return &s_rs; }
sys_regs_t          *gw_sys(void)    { return &s_sys; }

uint32_t gw_round_begin(uint8_t ch)
{
    xSemaphoreTake(s_round[ch - 1u], portMAX_DELAY);
    return s_table_gen;
}

void gw_round_end(uint8_t ch)
{
    xSemaphoreGive(s_round[ch - 1u]);
}

mb_ex_t gw_file_record(void *ctx, uint16_t file_no, uint16_t record_no, const uint8_t *data, uint16_t len)
{
    mb_ex_t ex;
    (void)ctx;

    ex = table_loader_record(&s_loader, file_no, record_no, data, len);
    if (s_loader.last_err != 0)
        s_sys.last_err = s_loader.last_err;     /* 已经在 gw_lock 里 */
    else if (s_loader.ready)
        s_sys.last_err = 0;                     /* 换上一张合法的新表：之前的错误清掉 */
    return ex;
}

void gw_apply_table(void)
{
    if (!s_loader.ready)
        return;

    LOGI("gw", "new point table received (%u points), waiting for channels to pause", (unsigned)s_loader.tmp.n);
    xSemaphoreTake(s_round[0], portMAX_DELAY);  /* 等 ch1 跑完这一轮 */
    xSemaphoreTake(s_round[1], portMAX_DELAY);  /* 等 ch2 跑完这一轮 */

    memcpy(&s_pt, &s_loader.tmp, sizeof(s_pt)); /* 两个通道都停着，up 任务自己也不在 mb_slave_handle 里 */
    s_loader.ready = 0;
    reg_space_load(&s_rs, &s_pt);               /* 内部会加 gw_lock */
    s_table_gen++;

    xSemaphoreGive(s_round[1]);
    xSemaphoreGive(s_round[0]);
    LOGI("gw", "point table applied: %u points, %u read blocks", (unsigned)s_pt.n, (unsigned)s_pt.nblocks);
}
