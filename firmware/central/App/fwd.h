/*
 * fwd.h - 转发：上位机经中控给 RS485 上的传感器发命令、升级固件（第 8b 步，纯逻辑）
 *
 * 两种请求由这里处理（在 up 任务里、不持有 gw_lock 的时候），其他请求照旧交给 mb_slave_handle：
 *
 *   06 写 0xF011 TARGET_CMD = v   → 把 v 写到目标传感器的 4x 寄存器 0（命令寄存器）
 *                                   课程传感器：0x55 = 进 Bootloader，0xAA = 启动 App（它们会先回应再复位）
 *                                   目标是系统区 0xF010 TARGET（高字节通道 1～2，低字节设备地址）
 *   15 file_no = 0x1000 | 通道 << 8 | 设备地址（通道 1～2）
 *                                 → 改成 file_no 1（传感器认的固件文件号）转发给那个传感器
 *
 * 课程传感器 Bootloader 收固件的方式（读了 bootloader.c、control.c，10/26）：
 *   record 0（文件头）：擦掉 App 区（按文件长度）和配置区，**擦完才回应**（几百毫秒）
 *   record 1、2……：**不检查序号**，收到一包就接着往下写；最后一包写完写配置区（"App 有效"）
 *   不检查 CRC；重复的包会被再写一次（数据就错位了）
 *
 * 所以中控这一侧要把好关：
 *   1. 文件头先不转发，等第 1 包来了、检查向量表像一个传感器 App（栈在 F030 的 RAM 里、
 *      入口在 0x08020000 之后），才先后转发文件头和第 1 包。发错文件（比如中控自己的固件）时传感器一点没动
 *   2. 按序号检查：跳包 → 拒绝；上位机重发刚转发成功的那一包 → 直接回成功，**不再转发**
 *   3. 某一包转发失败（不知道传感器写没写）→ 这次传输作废，之后的包都拒绝，必须从文件头重来
 *   4. 一边转发一边算 CRC32；**最后一包转发之前**核对整个文件的 CRC，不对就不发最后一包——
 *      传感器收不到最后一包就不会写"App 有效"，留在 Bootloader 里，不会启动一个坏的 App
 *   5. 文件头、命令可以重试（重新擦一遍 / 重复写命令都没有副作用）；数据包不重试
 *
 * 出错时 LAST_ERR = 0x06xx（见 FWD_ERR_*），回应异常 04（文件格式问题回 03）。
 *
 * PC 上测试：tests/test_fwd.c（模拟课程传感器的 Bootloader，包括它"重复包会写错"的行为）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FWD_H
#define FWD_H

#include <stdint.h>
#include "mb_rtu.h"

#define FWD_FILE_BASE       0x1000u
#define FWD_SYS_TARGET_CMD  0xF011u

/* 课程 F030 传感器 */
#define SENSOR_FILE_NO      1u
#define SENSOR_CMD_REG      0u
#define SENSOR_APP_ADDR     0x08020000u
#define SENSOR_APP_MAX      0x0001F800u     /* 到它的配置区 0x0803F800 为止 */
#define SENSOR_RAM_START    0x20000000u
#define SENSOR_RAM_END      0x20008000u     /* STM32F030CC：32 KB */

/* 等传感器回应的时间 */
#define FWD_HDR_TIMEOUT_MS  3000u           /* 文件头：传感器要先擦 Flash */
#define FWD_DATA_TIMEOUT_MS 1000u
#define FWD_CMD_TIMEOUT_MS  500u

/* LAST_ERR */
#define FWD_ERR_TARGET      0x0601u     /* TARGET 没设或不合法 */
#define FWD_ERR_NO_REPLY    0x0602u     /* 传感器没回应 */
#define FWD_ERR_EXCEPTION   0x0603u     /* 传感器回了异常码 */
#define FWD_ERR_SEQUENCE    0x0604u     /* 没有文件头就来了数据 / 跳包 */
#define FWD_ERR_BROKEN      0x0605u     /* 之前有一包失败了：从文件头重来 */
#define FWD_ERR_VECTORS     0x0606u     /* 第 1 包不像传感器 App（发错文件了？） */
#define FWD_ERR_CRC         0x0607u     /* 整个文件 CRC 不对：最后一包没发 */
#define FWD_ERR_LENGTH      0x0608u     /* 文件头里的长度不对 / 数据超出 */
#define FWD_ERR_REQUEST     0x0609u     /* 请求格式不对（多个子请求、文件头不是 32 字节） */

/* 往通道 ch 发一个请求、等回应。返回 MB_OK / MB_ERR_EXCEPTION（*ex 是异常码）/ 其他 mb_err_t */
typedef int (*fwd_transact_fn)(void *ctx, uint8_t ch, const uint8_t *req, uint16_t req_len,
                               uint32_t timeout_ms, uint8_t retries, mb_ex_t *ex);

typedef enum { FWD_IDLE = 0, FWD_HDR_HELD, FWD_SENDING, FWD_DONE, FWD_BROKEN } fwd_state_t;

typedef struct {
    fwd_transact_fn transact;
    void           *ctx;
    uint16_t      (*get_target)(void *ctx);
    void          (*set_err)(void *ctx, uint16_t err);

    /* 这次文件传输 */
    fwd_state_t state;
    uint16_t    file_no;            /* 上位机发来的（0x1CAA） */
    uint8_t     hdr[32];
    uint32_t    file_len, file_crc, crc_acc, sent;
    uint16_t    next_rec;           /* 期望的下一个 record_no */
    uint16_t    last_rec;           /* 上一个转发成功的 */

    /* 统计 */
    uint32_t    records_fwd, dup_skipped, cmds_fwd, errors;
} fwd_t;

void fwd_init(fwd_t *f, fwd_transact_fn transact, void *ctx,
              uint16_t (*get_target)(void *ctx), void (*set_err)(void *ctx, uint16_t err));

/* 处理一帧上位机请求。是转发请求：*handled = 1，返回回应长度（rsp 里是回应）；
 * 不是（或者 CRC 错、不是发给 my_addr 的）：*handled = 0，返回 0，调用者交给 mb_slave_handle */
uint16_t fwd_handle(fwd_t *f, uint8_t my_addr, const uint8_t *req, uint16_t req_len, uint8_t *rsp, int *handled);

const char *fwd_state_name(fwd_state_t s);

#endif /* FWD_H */
