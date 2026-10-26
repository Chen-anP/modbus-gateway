/*
 * gw_data.c - 中控的共享数据，说明见 gw_data.h
 *
 * 换点表的过程（gw_apply_table，在 up 任务里，回应已经发出去了）：
 *   1. 拿 ch1、ch2 两把轮锁：等两个通道各自跑完当前这一轮（最长约 1 秒：设备超时重试的情况）
 *   2. 把临时表复制成正在用的表，reg_space_load()：所有点回到"未同步"，待写入清掉
 *   3. 点表版本号 + 1，放开轮锁
 *   4. 通道任务下一轮开始时发现版本号变了，point_sync_init() 按新表重新找设备
 * 拿锁的顺序固定（先 ch1 后 ch2），up 任务在拿轮锁时不持有 gw_lock——避免互相等待（死锁）。
 *   5. （第 7a 步）换上以后把这份点表文件存进 Flash 点表区，再读回来核对；重启后开机就用它
 *
 * 开机（gw_data_init）选点表的顺序（第 7a 步）：
 *   Flash 点表区有合法的表 → 用它（PT_SOURCE = 1）
 *   Flash 是空的           → 内置默认表（PT_SOURCE = 0）
 *   Flash 里的表坏了       → 内置默认表（PT_SOURCE = 4），LAST_ERR = 0x0402 / 0x0403
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "log.h"
#include "version.h"
#include "default_points.h"
#include "table_loader.h"
#include "poll_task.h"
#include "flash_layout.h"
#include "flash_if.h"
#include "pt_store.h"
#include "boot_cfg.h"
#include "gw_data.h"

static point_table_t     s_pt;
static reg_space_t       s_rs;
static sys_regs_t        s_sys;
static table_loader_t    s_loader;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_round[PT_CH_COUNT];
static volatile uint32_t s_table_gen;           /* 点表版本号，每换一次 + 1 */
static uint8_t           s_inited;
static uint32_t          s_pt_seq;              /* Flash 里点表的保存次数 */

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

/* 开机：先试 Flash 里保存的表，不行再用内置默认表 */
static void load_initial_table(void)
{
    static uint8_t buf[PT_MAX_POINTS * PT_REC_SIZE];
    const uint8_t *fdata;
    uint32_t len, seq = 0;
    uint16_t bad = 0;
    pt_err_t e;
    pts_result_t r = pt_store_load(flash_if_io(), FL_PT_ADDR, &fdata, &len, &seq);

    if (r == PTS_OK) {
        e = point_table_parse(&s_pt, fdata, len, &bad);
        if (e == PT_OK) {
            s_pt_seq = seq;
            s_sys.pt_source = SYS_PT_FLASH;
            LOGI("gw", "point table: %u points, %u read blocks (flash, save #%lu)",
                 (unsigned)s_pt.n, (unsigned)s_pt.nblocks, (unsigned long)seq);
            return;
        }
        s_pt_seq = seq;                     /* 下次保存的次数接着往上数 */
        s_sys.pt_source = SYS_PT_FLASH_BAD;
        s_sys.last_err = SYS_ERR_PT_INVALID;
        LOGE("gw", "saved point table invalid: %s at point %u, using built-in", pt_err_name(e), (unsigned)bad);
    } else if (r == PTS_BAD_DATA) {
        s_sys.pt_source = SYS_PT_FLASH_BAD;
        s_sys.last_err = SYS_ERR_PT_CRC;
        LOGE("gw", "saved point table: crc error, using built-in");
    } else {
        s_sys.pt_source = SYS_PT_BUILTIN;   /* 空的：正常情况，没下发过 */
    }

    len = default_points(buf, sizeof(buf));
    e = point_table_parse(&s_pt, buf, len, &bad);
    if (e != PT_OK) {                       /* 默认表写错了：不映射任何点，LAST_ERR 记下原因 */
        s_pt.n = 0;
        s_pt.nblocks = 0;
        s_sys.last_err = (uint16_t)(0x0100u | (unsigned)e);
        LOGE("gw", "default point table error: %s at point %u", pt_err_name(e), (unsigned)bad);
    }
    LOGI("gw", "point table: %u points, %u read blocks (built-in)", (unsigned)s_pt.n, (unsigned)s_pt.nblocks);
}

static void log_boot_cfg(void)
{
    boot_cfg_t c;

    if (!boot_cfg_read(flash_if_io(), FL_CFG_ADDR, &c))
        LOGI("gw", "boot config: none");
    else
        LOGI("gw", "boot config: app len %lu crc %08lX ver %lu, stay %lu",
             (unsigned long)c.app_len, (unsigned long)c.app_crc, (unsigned long)c.app_ver, (unsigned long)c.stay);
}

void gw_data_init(void)
{
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

    flash_if_set_window(FL_PT_ADDR, FL_BASE + FL_SIZE);   /* App 只能擦写点表区和配置区 */
    flash_if_log_option_bytes();
    log_boot_cfg();
    load_initial_table();

    reg_space_load(&s_rs, &s_pt);
    s_rs.lock = gw_lock;                    /* 之后多个任务会同时用，装上锁 */
    s_rs.unlock = gw_unlock;
    s_table_gen = 1;
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

/* 把刚换上的点表文件存进 Flash，再读回来核对。在 up 任务里、不持有任何锁时调用 */
static void save_table(const uint8_t *data, uint32_t len)
{
    const flash_if_stats_t *fs = flash_if_stats();
    const uint8_t *back;
    uint32_t blen, bseq, t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    pts_result_t r = pt_store_save(flash_if_io(), FL_PT_ADDR, data, len, s_pt_seq + 1u);

    if (r == PTS_OK) {                      /* 读回来核对：CRC 对、内容一字不差 */
        r = pt_store_load(flash_if_io(), FL_PT_ADDR, &back, &blen, &bseq);
        if (r == PTS_OK && (blen != len || memcmp(back, data, len) != 0))
            r = PTS_BAD_DATA;
    }
    gw_lock();
    if (r == PTS_OK) {
        s_pt_seq++;
        s_sys.pt_source = SYS_PT_SAVED;
    } else {
        s_sys.pt_source = SYS_PT_NOT_SAVED;
        s_sys.last_err = SYS_ERR_PT_SAVE;
    }
    gw_unlock();
    if (r == PTS_OK)
        LOGI("gw", "point table saved to flash: %lu bytes, save #%lu, %lu ms (erase %lu ms)",
             (unsigned long)len, (unsigned long)s_pt_seq,
             (unsigned long)((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0), (unsigned long)fs->last_erase_ms);
    else
        LOGE("gw", "point table save FAILED: %s (hal errors %lu, last 0x%08lX, refused %lu)", pts_name(r),
             (unsigned long)fs->hal_errors, (unsigned long)fs->last_hal_error, (unsigned long)fs->refused);
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

    save_table(s_loader.buf, s_loader.rx.hdr.length);
}

int gw_clear_saved_table(void)
{
    pts_result_t r = pt_store_clear(flash_if_io(), FL_PT_ADDR);

    LOGW("gw", "saved point table cleared: %s", pts_name(r));
    return r == PTS_OK ? 0 : -1;
}

int gw_set_stay_in_boot(int stay)
{
    boot_cfg_t c;
    int rc;

    if (!boot_cfg_read(flash_if_io(), FL_CFG_ADDR, &c)) {
        if (!stay) {                        /* 没有配置 = 本来就不会留下，不用写 */
            LOGI("gw", "boot config: none, nothing to clear");
            return 0;
        }
        memset(&c, 0, sizeof(c));           /* app_len = 0：没有 App 信息（第 7b 步升级时才写） */
    }
    if (c.stay == (uint32_t)(stay ? 1 : 0)) {
        LOGI("gw", "boot config: stay already %u", (unsigned)c.stay);
        return 0;                           /* 已经是这样了，不擦写（Flash 擦写次数有限） */
    }
    c.stay = stay ? 1u : 0u;
    rc = boot_cfg_write(flash_if_io(), FL_CFG_ADDR, &c);
    if (rc != 0) {
        gw_lock();
        s_sys.last_err = SYS_ERR_CFG_WRITE;
        gw_unlock();
    }
    LOGW("gw", "boot config: stay = %u %s (erase %lu ms)", (unsigned)c.stay, rc == 0 ? "ok" : "FAILED",
         (unsigned long)flash_if_stats()->last_erase_ms);
    return rc;
}
