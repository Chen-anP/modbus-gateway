/*
 * boot_proto.h - Bootloader 的 Modbus 从机：系统区寄存器 + 0x15 收固件
 *
 * 地址、功能码和 App 一样（中控地址 1，系统区 4x 0xF000 起），上位机工具读 STATE 就知道现在在哪边：
 *
 *   地址     读 / 写   含义（Bootloader 里）
 *   0xF000   写        CMD：0x0001 重启（还会回到 Bootloader）；0x0A00 清掉 stay 并重启 → 启动 App；
 *                           0x0A01 清掉配置区里的 App 长度和 CRC 再启动（用 Keil 下载过 App 以后，配置区里
 *                                  记的还是上次升级的 CRC，对不上会一直留在 Bootloader，用它"认下"现在的 App）；
 *                           0x0B00 已经在 Bootloader 了，接受但什么都不做；其他回异常 03
 *   0xF001   读        STATE = 1（在 Bootloader）
 *   0xF002   读        FW_VER = Bootloader 的版本
 *   0xF003   读        POINT_CNT = 0
 *   0xF004   读        LAST_ERR：0x03xx 文件传输错误，0x05xx 升级错误（低字节是 af_err_t）
 *   0xF005   读        BOOT_REASON：为什么留在 Bootloader（boot_decision_t）
 *   0xF006   读        UPG_STATE：0 空闲 / 1 正在收 / 2 收齐了、正在写 Flash / 3 写好了、等 0x0A00 / 4 失败
 *   0xF007   读        APP_VER：配置区里记的 App 版本（低 16 位）
 *
 *   0x15 file_no 0x1000 = 中控自己的固件（设计文档 4.3 节），其他 file_no 回异常 02
 *
 * 收齐、CRC 对、app_flash_check 通过 → 回成功，记下"要写 Flash"；主循环在回应发出去以后调用
 * app_flash_program()，再调用 boot_proto_program_done() 记结果。上位机轮询 UPG_STATE。
 *
 * 纯逻辑，PC 上测试：tests/test_boot_proto.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BOOT_PROTO_H
#define BOOT_PROTO_H

#include <stdint.h>
#include "mb_rtu.h"
#include "file_rx.h"
#include "app_flash.h"

#define FILE_NO_CENTRAL_FW  0x1000u

#define BP_REG_CMD          0xF000u
#define BP_REG_LAST         0xF007u     /* 0xF000～0xF007 */

#define BP_CMD_REBOOT       0x0001u
#define BP_CMD_RUN_APP      0x0A00u
#define BP_CMD_FORGET_APP   0x0A01u
#define BP_CMD_ENTER_BOOT   0x0B00u

/* UPG_STATE */
enum { BP_UPG_IDLE = 0, BP_UPG_RECEIVING, BP_UPG_PROGRAMMING, BP_UPG_DONE, BP_UPG_FAILED };

/* 主循环要做的事（回应发出去以后） */
enum { BP_ACT_NONE = 0, BP_ACT_PROGRAM, BP_ACT_REBOOT, BP_ACT_RUN_APP, BP_ACT_FORGET_APP };

#define BP_ERR_FILE         0x0300u     /* + frx_err_t */
#define BP_ERR_UPGRADE      0x0500u     /* + af_err_t */

typedef struct {
    file_rx_t   rx;
    af_layout_t layout;
    uint16_t    boot_ver;
    uint16_t    reason;         /* boot_decision_t */
    uint16_t    app_ver;
    uint16_t    last_err;
    uint16_t    upg_state;
    uint16_t    action;
} boot_proto_t;

void boot_proto_init(boot_proto_t *b, uint8_t *buf, uint32_t cap, const af_layout_t *layout,
                     uint16_t boot_ver, uint16_t reason, uint16_t app_ver);

void boot_proto_ops(boot_proto_t *b, mb_slave_ops_t *ops);

/* 取走要做的事（取完清零） */
uint16_t boot_proto_take_action(boot_proto_t *b);

/* 写 Flash 的结果 */
void boot_proto_program_done(boot_proto_t *b, af_err_t e);

#endif /* BOOT_PROTO_H */
