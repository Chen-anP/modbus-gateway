/*
 * boot_cfg.h - 配置区（0x081FE000，8 KB）：Bootloader 开机时靠它决定"跳到 App"还是"留在 Bootloader"
 *
 * 第 7a 步先把格式、读写和判断逻辑做好、在 PC 上测透；App 只用它来设置 / 清除"留在 Bootloader"标志。
 * 第 7b 步 Bootloader 开机时调用 boot_cfg_decide()，升级完成后写入新的 App 长度和 CRC。
 *
 * 格式：32 字节，8 个 32 位字（小端）
 *   +0   魔数 'GWCF'（0x47574346）
 *   +4   app_len     App 的长度（字节）；0 = 没有 App 信息（第 7a 步 App 只设 stay 标志时就是这样）
 *   +8   app_crc     App 的 CRC32
 *   +12  app_ver     App 版本（给人看）
 *   +16  stay        1 = 下次开机留在 Bootloader（上位机发 0x0B00 时设置）
 *   +20  保留 ×2
 *   +28  self_crc    前 28 字节的 CRC32
 *
 * 写的方法：擦扇区 → 写 32 字节。中间断电 → 配置区是空的 → Bootloader 按"没有配置"处理（见 decide）。
 *
 * 纯逻辑，PC 上测试：tests/test_boot_cfg.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BOOT_CFG_H
#define BOOT_CFG_H

#include <stdint.h>
#include "flash_io.h"

#define BOOT_CFG_MAGIC  0x47574346u     /* 'GWCF' */
#define BOOT_CFG_SIZE   32u

typedef struct {
    uint32_t magic;
    uint32_t app_len;
    uint32_t app_crc;
    uint32_t app_ver;
    uint32_t stay;
    uint32_t rsv[2];
    uint32_t self_crc;
} boot_cfg_t;

/* Bootloader 开机的判断结果 */
typedef enum {
    BOOT_JUMP = 0,          /* 配置有效、App 的 CRC 对、向量表合理：跳过去 */
    BOOT_JUMP_NO_CFG,       /* 没有配置（或 app_len = 0），但 App 的向量表合理：用调试器直接下载的 App，不查 CRC 直接跳 */
    BOOT_STAY_REQUEST,      /* 上位机要求留下（stay = 1） */
    BOOT_STAY_NO_APP,       /* 没有配置，向量表也不合理：App 区是空的或者没写完 */
    BOOT_STAY_BAD_LEN,      /* 配置里的长度小于 8（放不下向量表）或者超出 App 区 */
    BOOT_STAY_BAD_CRC,      /* App 的 CRC 和配置里的对不上：升级没完成、Flash 坏了 */
    BOOT_STAY_BAD_VEC,      /* CRC 对，但向量表不合理（下载了别的地址编译的固件） */
    BOOT_STAY_BOOTLOOP      /* App 连续几次启动都没"确认"就复位了（第 8 步，boot_guard.h）；不是 decide 给的，Bootloader 自己判断 */
} boot_decision_t;

/* 读配置区。有效（魔数、自己的 CRC 都对）返回 1，*out 填好；否则返回 0 */
int boot_cfg_read(const flash_io_t *io, uint32_t addr, boot_cfg_t *out);

/* 写配置区：补上魔数和 self_crc，擦扇区，写 32 字节。成功 0 */
int boot_cfg_write(const flash_io_t *io, uint32_t addr, boot_cfg_t *cfg);

/* 向量表的头两项（初始栈指针、复位入口）是否合理。Bootloader 收到新固件、写 Flash 之前也用它检查 */
int boot_vectors_ok(uint32_t sp, uint32_t reset, uint32_t app_addr, uint32_t app_len,
                    uint32_t ram_start, uint32_t ram_end);

/* App 开头的向量表是否合理：初始栈指针在 RAM 里、8 字节对齐；复位入口是 Thumb 地址（最低位 1）且在 App 里面 */
int boot_app_vectors_ok(const flash_io_t *io, uint32_t app_addr, uint32_t app_len,
                        uint32_t ram_start, uint32_t ram_end);

/* 开机判断。cfg = NULL 表示配置区无效。app_max = App 区的大小 */
boot_decision_t boot_cfg_decide(const flash_io_t *io, const boot_cfg_t *cfg, uint32_t app_addr, uint32_t app_max,
                                uint32_t ram_start, uint32_t ram_end);

const char *boot_decision_name(boot_decision_t d);

#endif /* BOOT_CFG_H */
