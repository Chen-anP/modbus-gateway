/*
 * mb_rtu.h - 轻量 Modbus RTU 协议层（主从一体）
 *
 * 只做协议：组帧、CRC、按功能码算帧长、从机请求处理、主机请求生成与回应解析。
 * 不依赖 HAL / RTOS / 动态内存，可以在 PC 上编译测试（tests/test_mb_rtu.c）。
 *
 * 帧结束判断用"按功能码算长度"而不是 3.5 字符间隔：
 *   - 中控上行是 USB 虚拟串口，USB 按包传输，根本没有字符间隔可言；
 *   - 下行 RS485 用 DMA + 空闲中断收数据，一帧可能分几次到达。
 * 两种情况都可以用 mb_frame_need() 判断"这一帧还差多少字节"，
 * 再配合一个字节间超时丢弃残帧（见 mb_framer_*）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef MB_RTU_H
#define MB_RTU_H

#include <stdint.h>

#define MB_ADU_MAX          256u    /* 地址 1 + PDU 253 + CRC 2 */
#define MB_ADDR_BROADCAST   0u

/* 功能码 */
#define MB_FC_READ_COILS            0x01
#define MB_FC_READ_DISCRETE         0x02
#define MB_FC_READ_HOLDING          0x03
#define MB_FC_READ_INPUT            0x04
#define MB_FC_WRITE_COIL            0x05
#define MB_FC_WRITE_REG             0x06
#define MB_FC_WRITE_COILS           0x0F
#define MB_FC_WRITE_REGS            0x10
#define MB_FC_WRITE_FILE_RECORD     0x15

/* 异常码 */
typedef enum {
    MB_EX_NONE              = 0x00,
    MB_EX_ILLEGAL_FUNCTION  = 0x01,
    MB_EX_ILLEGAL_ADDRESS   = 0x02,
    MB_EX_ILLEGAL_VALUE     = 0x03,
    MB_EX_DEVICE_FAILURE    = 0x04,
    MB_EX_BUSY              = 0x06
} mb_ex_t;

/* 四种数据表 */
typedef enum {
    MB_TAB_COIL = 0,        /* 0x 线圈，可读写 */
    MB_TAB_DISCRETE,        /* 1x 离散输入，只读 */
    MB_TAB_INPUT,           /* 3x 输入寄存器，只读 */
    MB_TAB_HOLDING,         /* 4x 保持寄存器，可读写 */
    MB_TAB_COUNT
} mb_table_t;

/* 主机侧结果 */
typedef enum {
    MB_OK          = 0,
    MB_ERR_TIMEOUT = -1,    /* 链路层使用：超时没收到完整回应 */
    MB_ERR_CRC     = -2,
    MB_ERR_FRAME   = -3,    /* 地址 / 功能码 / 长度 / 回显内容对不上 */
    MB_ERR_EXCEPTION = -4,  /* 从机回了异常码，见 ex */
    MB_ERR_PARAM   = -5     /* 调用参数非法（数量越界等） */
} mb_err_t;

/* ---------------------------------------------------------------- 通用 */

uint16_t mb_crc16(const uint8_t *data, uint16_t len);

/* 已经收到 have 个字节，判断整帧应该有多长。
 *   is_request = 1：按"请求帧"格式算（从机用）；0：按"回应帧"格式算（主机用）
 * 返回值：>0 整帧长度（含 CRC）；0 还需要更多字节才能判断；<0 功能码不认识或长度字段非法 */
int mb_frame_need(const uint8_t *buf, uint16_t have, int is_request);

/* ---------------------------------------------------------------- 帧组装器
 * 链路层把收到的字节一个个（或一段段）喂进来，凑够一帧就返回 1。
 * 两个字节之间间隔超过 gap_ms，就丢弃已收到的半帧（对应规范里的帧间隔）。
 */
typedef struct {
    uint8_t  buf[MB_ADU_MAX];
    uint16_t len;
    uint32_t last_ms;
    uint32_t gap_ms;
    uint8_t  is_request;
    uint32_t dropped;       /* 统计：被丢弃的残帧 / 非法帧数量 */
} mb_framer_t;

void mb_framer_init(mb_framer_t *f, int is_request, uint32_t gap_ms);
void mb_framer_reset(mb_framer_t *f);
/* 喂入数据。返回：1 已凑成一帧（f->buf / f->len，调用者处理完后调用 mb_framer_reset）；
 *               0 还没收完；消耗掉的字节数通过 *used 返回（一帧收完后剩下的字节下次再喂） */
int mb_framer_feed(mb_framer_t *f, const uint8_t *data, uint16_t len, uint32_t now_ms, uint16_t *used);

/* ---------------------------------------------------------------- 从机
 * 寄存器访问通过回调交给上层（中控的映射表、传感器的寄存器表都实现这一组回调）。
 * 处理顺序：先调用 check 检查整段地址是否合法，再逐个读写——
 * 这样多寄存器写入要么全部成功，要么一个都不写。
 */
typedef struct {
    void *ctx;
    /* 地址范围检查：返回 MB_EX_NONE 或 MB_EX_ILLEGAL_ADDRESS */
    mb_ex_t (*check)(void *ctx, mb_table_t tab, uint16_t addr, uint16_t count, int is_write);
    mb_ex_t (*read)(void *ctx, mb_table_t tab, uint16_t addr, uint16_t *value);
    mb_ex_t (*write)(void *ctx, mb_table_t tab, uint16_t addr, uint16_t value);
    /* 0x15 每个子请求调用一次；为 NULL 表示不支持 0x15 */
    mb_ex_t (*file_record)(void *ctx, uint16_t file_no, uint16_t record_no,
                           const uint8_t *data, uint16_t len);
} mb_slave_ops_t;

/* 处理一帧完整请求（含 CRC），在 rsp 里生成回应。
 * 返回回应长度；返回 0 表示不回复（CRC 错、不是发给本机的、广播）。 */
uint16_t mb_slave_handle(uint8_t my_addr, const mb_slave_ops_t *ops,
                         const uint8_t *req, uint16_t req_len, uint8_t *rsp);

/* ---------------------------------------------------------------- 主机
 * build 系列返回请求帧长度（含 CRC），参数非法返回 0。 */
uint16_t mb_build_read(uint8_t *adu, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t count);
uint16_t mb_build_write_single(uint8_t *adu, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t value);
uint16_t mb_build_write_regs(uint8_t *adu, uint8_t slave, uint16_t addr, uint16_t count, const uint16_t *values);
uint16_t mb_build_file_record(uint8_t *adu, uint8_t slave, uint16_t file_no, uint16_t record_no,
                              const uint8_t *data, uint16_t len);

/* 解析回应。读请求的结果写到 values（线圈 / 离散量每个点占一个元素，值为 0/1）。
 * 返回 MB_OK 或 mb_err_t；从机异常时返回 MB_ERR_EXCEPTION，异常码写到 *ex。 */
int mb_parse_rsp(const uint8_t *req, uint16_t req_len, const uint8_t *rsp, uint16_t rsp_len,
                 uint16_t *values, uint16_t max_values, mb_ex_t *ex);

#endif /* MB_RTU_H */
