/*
 * app_flash.h - Bootloader 把收到的新固件写进 App 区（单区覆盖）
 *
 * 固件先整个收进 RAM、CRC32 校验通过、向量表检查通过，才开始动 Flash（传输中断不会影响旧 App）。
 * 写的顺序，每一步断电都安全（tests/test_app_flash.c 每一步都断电测过）：
 *
 *   1. 配置区写成 stay = 1、app_len = 0     ← 之后任何时候断电，开机都留在 Bootloader
 *   2. 擦 App 用到的扇区（从第一个扇区开始）
 *   3. 写第 16 字节以后的内容
 *   4. 核对 Flash 里的 CRC（前 16 字节用 RAM 里的）
 *   5. 最后写前 16 字节（初始栈指针 + 复位入口）
 *   6. 核对整个 App 的 CRC
 *   7. 配置区写成 stay = 1、app_len、app_crc、app_ver   ← 等上位机发 0x0A00 才启动新 App
 *
 * 断电在第 1 步写完之前：配置区可能是空的，但旧 App 还完整 → 开机跳旧 App（不查 CRC）
 * 断电在第 7 步写完之前：配置区可能是空的，但新 App 已经完整、向量表最后才写 → 跳新 App 或留下
 *
 * 纯逻辑：通过 flash_io_t 访问 Flash，PC 上用假 Flash 测。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef APP_FLASH_H
#define APP_FLASH_H

#include <stdint.h>
#include "flash_io.h"

typedef enum {
    AF_OK = 0,
    AF_ERR_ADDR,        /* 文件头里的加载地址不是 App 区（编译时地址没改？） */
    AF_ERR_SIZE,        /* 太大或者太小 */
    AF_ERR_VECTORS,     /* 向量表不合理 */
    AF_ERR_ERASE,
    AF_ERR_WRITE,
    AF_ERR_VERIFY,      /* 写完读回来 CRC 不对 */
    AF_ERR_CFG          /* 写配置区失败 */
} af_err_t;

typedef struct {
    uint32_t app_addr, app_max;     /* App 区 */
    uint32_t cfg_addr;              /* 配置区 */
    uint32_t ram_start, ram_end;    /* 检查初始栈指针用 */
} af_layout_t;

/* 写 Flash 之前的检查（不碰 Flash）：加载地址、长度、向量表 */
af_err_t app_flash_check(const af_layout_t *l, const uint8_t *img, uint32_t len, uint32_t load_addr);

/* 按上面 7 步写进去。crc = 整个固件的 CRC32（文件头里的，已经和 RAM 里的内容核对过） */
af_err_t app_flash_program(const flash_io_t *io, const af_layout_t *l,
                           const uint8_t *img, uint32_t len, uint32_t crc, uint32_t ver);

const char *af_err_name(af_err_t e);

#endif /* APP_FLASH_H */
