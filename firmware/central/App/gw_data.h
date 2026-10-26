/*
 * gw_data.h - 中控的共享数据：点表、寄存器空间、系统区、点表下发和保存，以及保护它们的锁
 *
 * up 任务（上位机读写、下发点表）和 ch1 / ch2 任务（同步传感器）都要访问这些数据，
 * 这里集中创建，大家通过下面的函数拿指针。
 *
 * 两种锁：
 *   gw_lock / gw_unlock        寄存器空间的锁，持有时间只有微秒级，锁里不做收发
 *   gw_round_begin / end       每个通道一把"轮锁"：通道任务每一轮都持有它（轮里有串口收发）；
 *                              换点表时 up 任务把两把轮锁都拿到，说明两个通道都停在两轮之间，才能安全换表
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef GW_DATA_H
#define GW_DATA_H

#include <stdint.h>
#include "mb_rtu.h"
#include "point_table.h"
#include "reg_space.h"
#include "sys_regs.h"

/* 创建锁、加载默认点表。可以重复调用（只做一次）。在调度器启动前调用 */
void gw_data_init(void);

const point_table_t *gw_points(void);
reg_space_t         *gw_rs(void);
sys_regs_t          *gw_sys(void);

/* 寄存器空间的锁（up 任务包住 mb_slave_handle 时用） */
void gw_lock(void);
void gw_unlock(void);

/* 通道任务每一轮的开始和结束。返回点表的版本号：和上一轮不同，说明点表换过了，要重新初始化 */
uint32_t gw_round_begin(uint8_t ch);
void     gw_round_end(uint8_t ch);

/* 0x15 回调（在 mb_slave_handle 里、gw_lock 内调用）：交给 table_loader */
mb_ex_t gw_file_record(void *ctx, uint16_t file_no, uint16_t record_no, const uint8_t *data, uint16_t len);

/* up 任务回完一帧后调用：如果收到了一张合法的新表，等两个通道停下来，换上，再存进 Flash。没有新表立刻返回 */
void gw_apply_table(void);

/* 第 8b 步：在通道 ch（1、2）上发一个请求、等回应（fwd.c 的 transact）。up 任务里、不持有 gw_lock 时调用：
 * 拿这个通道的轮锁（通道任务停在两轮之间），临时改超时和重试次数，用通道自己的 mb_master 收发 */
int gw_forward(void *ctx, uint8_t ch, const uint8_t *req, uint16_t req_len,
               uint32_t timeout_ms, uint8_t retries, mb_ex_t *ex);

/* 第 7a 步的命令（up 任务在回应发出去以后调用，不持有任何锁）。成功 0 */
int gw_clear_saved_table(void);         /* 擦掉 Flash 里的点表：下次开机用内置默认表 */
int gw_set_stay_in_boot(int stay);      /* 配置区的"下次开机留在 Bootloader"标志 */

#endif /* GW_DATA_H */
