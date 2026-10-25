/*
 * sys_regs.h - 中控自己的"系统区"寄存器（保持寄存器 4x 0xF000～0xF0FF），上位机通过 USB 读写
 *
 * 设计文档 4.2 节的系统区，第 4 步先实现这些：
 *
 *   地址     读 / 写   含义
 *   0xF000   写        CMD：0x0001 = 重启（其他命令第 7、8 步加）；读出来总是 0
 *   0xF001   读        STATE：0 = 运行 App（第 7 步 Bootloader 里是 1）
 *   0xF002   读        FW_VER：固件版本
 *   0xF003   读        POINT_CNT：点表点数（第 5 步才有点表，现在是 0）
 *   0xF004   读        LAST_ERR：最近一次错误码（现在是 0）
 *   0xF020 + 4 ×（通道 − 1） + 0～3   读   每通道统计：成功、超时、CRC 错、掉线设备数（取低 16 位）
 *
 * 其他地址、其他三张表（0x / 1x / 3x）都回异常 02。映射区（四张表的 0～499）第 5 步由点表决定。
 *
 * 纯逻辑：统计通过函数指针取，命令只记下来不执行（重启要等回应发出去以后，由 up_link 做），
 * 所以可以在 PC 上测（tests/test_sys_regs.c）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef SYS_REGS_H
#define SYS_REGS_H

#include <stdint.h>
#include "mb_rtu.h"

#define SYS_REG_CMD         0xF000u
#define SYS_REG_STATE       0xF001u
#define SYS_REG_FW_VER      0xF002u
#define SYS_REG_POINT_CNT   0xF003u
#define SYS_REG_LAST_ERR    0xF004u
#define SYS_REG_CH_STAT     0xF020u     /* 每通道 4 个：ok、timeout、crc、offline */
#define SYS_CH_COUNT        2u

#define SYS_CMD_NONE        0x0000u
#define SYS_CMD_REBOOT      0x0001u

/* 一个通道的统计（32 位计数，读的时候取低 16 位，65535 之后回到 0） */
typedef struct {
    uint32_t ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t offline;       /* 现在掉线的设备数 */
} sys_ch_stat_t;

typedef struct {
    uint16_t fw_ver;
    uint16_t state;
    uint16_t point_cnt;
    uint16_t last_err;
    void   (*get_ch_stat)(uint8_t ch, sys_ch_stat_t *out);    /* ch = 1、2；NULL 时统计读出来都是 0 */
    uint16_t pending_cmd;   /* 上位机写进来、还没执行的命令 */
} sys_regs_t;

void sys_regs_init(sys_regs_t *s, uint16_t fw_ver, void (*get_ch_stat)(uint8_t ch, sys_ch_stat_t *out));

/* 填好一组从机回调，交给 mb_slave_handle() 用 */
void sys_regs_ops(sys_regs_t *s, mb_slave_ops_t *ops);

/* 取走待执行的命令（取完清零）。没有返回 SYS_CMD_NONE */
uint16_t sys_regs_take_cmd(sys_regs_t *s);

#endif /* SYS_REGS_H */
