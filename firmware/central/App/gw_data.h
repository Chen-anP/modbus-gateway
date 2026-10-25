/*
 * gw_data.h - 中控的共享数据：点表、寄存器空间、系统区，以及保护它们的互斥锁
 *
 * up 任务（上位机读写）和 ch1 / ch2 任务（同步传感器）都要访问寄存器空间，
 * 这里集中创建，大家通过下面的函数拿指针。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef GW_DATA_H
#define GW_DATA_H

#include "point_table.h"
#include "reg_space.h"
#include "sys_regs.h"

/* 创建互斥锁、加载默认点表。可以重复调用（只做一次）。在调度器启动前调用 */
void gw_data_init(void);

const point_table_t *gw_points(void);
reg_space_t         *gw_rs(void);
sys_regs_t          *gw_sys(void);

/* 寄存器空间的锁（up 任务包住 mb_slave_handle 时用） */
void gw_lock(void);
void gw_unlock(void);

#endif /* GW_DATA_H */
