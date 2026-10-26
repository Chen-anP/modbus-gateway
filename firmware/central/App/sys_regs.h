/*
 * sys_regs.h - 中控自己的"系统区"寄存器（保持寄存器 4x 0xF000～0xF0FF），上位机通过 USB 读写
 *
 * 设计文档 4.2 节的系统区：
 *
 *   地址     读 / 写   含义
 *   0xF000   写        CMD（读出来总是 0）：
 *                        0x0001 重启
 *                        0x0002 清掉 Flash 里保存的点表，然后重启（重启后用内置默认表）       第 7a 步
 *                        0x0B00 下次开机留在 Bootloader（写配置区 stay = 1），然后重启        第 7a 步
 *                        0x0A00 清掉"留在 Bootloader"标志（不重启；Bootloader 里还会跳到 App） 第 7a 步
 *                        0x0F01 故障测试：ch1 任务故意卡住，看门狗应该复位中控                 第 8 步
 *                        0x0F02 故障测试：设"开机就死机"标志并重启，连续 3 次后留在 Bootloader   第 8 步
 *   0xF001   读        STATE：0 = 运行 App（第 7b 步 Bootloader 里是 1）
 *   0xF002   读        FW_VER：固件版本
 *   0xF003   读        POINT_CNT：点表点数（第 5 步起由 reg_space_load 设置）
 *   0xF004   读        LAST_ERR：最近一次错误码：
 *                        0x01xx 点表错误（低字节是 pt_err_t）   0x0200 设备拒绝写入   0x03xx 文件传输错误
 *                        0x0401 点表存 Flash 失败   0x0402 Flash 里的点表 CRC 不对   0x0403 Flash 里的点表不合法
 *                        0x0404 写配置区失败
 *   0xF005   读        PT_SOURCE：现在的点表从哪来（见 SYS_PT_*）                                第 7a 步
 *   0xF006   读        RESET_REASON：上次复位的原因（rr_t：1 上电 2 复位键 3 软件 4 看门狗）       第 8 步
 *   0xF007   读        BOOT_TRIES：这次开机时的启动次数（稳定 5 秒后备份寄存器里清零，这里不变）   第 8 步
 *   0xF010   读写      TARGET：转发目标，高字节通道（1、2），低字节设备地址（1～247）               第 8b 步
 *   0xF011   写        TARGET_CMD：写到目标传感器的 4x 寄存器 0（由 fwd.c 在 up 任务里处理，不经过这里）
 *                      读出来总是 0；用 0x10 写多个寄存器写到它 → 异常 02
 *   0xF020 + 4 ×（通道 − 1） + 0～3   读   每通道统计：成功、超时、CRC 错、掉线设备数（取低 16 位）
 *
 * 第 5 步起系统区由 reg_space 转交过来；这个模块本身只管 0xF000 起的地址，其他一律回异常 02。
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
#define SYS_REG_PT_SOURCE   0xF005u
#define SYS_REG_RESET_RSN   0xF006u
#define SYS_REG_BOOT_TRIES  0xF007u
#define SYS_REG_TARGET      0xF010u
#define SYS_REG_TARGET_CMD  0xF011u
#define SYS_REG_CH_STAT     0xF020u     /* 每通道 4 个：ok、timeout、crc、offline */
#define SYS_CH_COUNT        2u

#define SYS_CMD_NONE        0x0000u
#define SYS_CMD_REBOOT      0x0001u
#define SYS_CMD_CLEAR_TABLE 0x0002u
#define SYS_CMD_ENTER_BOOT  0x0B00u
#define SYS_CMD_CLEAR_STAY  0x0A00u
#define SYS_CMD_TEST_HANG   0x0F01u
#define SYS_CMD_TEST_CRASH  0x0F02u

/* PT_SOURCE：现在的点表从哪来 */
#define SYS_PT_BUILTIN      0u      /* Flash 里没有保存的表，用内置默认表 */
#define SYS_PT_FLASH        1u      /* 开机从 Flash 读到的 */
#define SYS_PT_SAVED        2u      /* 这次运行中下发的，已经存进 Flash */
#define SYS_PT_NOT_SAVED    3u      /* 这次运行中下发的，但存 Flash 失败（重启后会丢） */
#define SYS_PT_FLASH_BAD    4u      /* Flash 里的表坏了，用的是内置默认表 */

/* LAST_ERR 里 Flash 相关的错误 */
#define SYS_ERR_PT_SAVE     0x0401u
#define SYS_ERR_PT_CRC      0x0402u
#define SYS_ERR_PT_INVALID  0x0403u
#define SYS_ERR_CFG_WRITE   0x0404u

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
    uint16_t pt_source;     /* SYS_PT_* */
    uint16_t reset_reason;  /* rr_t（boot_guard.h） */
    uint16_t boot_tries;
    uint16_t target;        /* 转发目标：通道 << 8 | 地址，0 = 没设 */
    void   (*get_ch_stat)(uint8_t ch, sys_ch_stat_t *out);    /* ch = 1、2；NULL 时统计读出来都是 0 */
    uint16_t pending_cmd;   /* 上位机写进来、还没执行的命令 */
} sys_regs_t;

void sys_regs_init(sys_regs_t *s, uint16_t fw_ver, void (*get_ch_stat)(uint8_t ch, sys_ch_stat_t *out));

/* 填好一组从机回调，交给 mb_slave_handle() 用 */
void sys_regs_ops(sys_regs_t *s, mb_slave_ops_t *ops);

/* 取走待执行的命令（取完清零）。没有返回 SYS_CMD_NONE */
uint16_t sys_regs_take_cmd(sys_regs_t *s);

#endif /* SYS_REGS_H */
