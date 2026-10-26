/*
 * flash_layout.h - STM32H563 的 Flash 分区（Bootloader 和 App 共用这一份）
 *
 *   2 MB = Bank 1（0x08000000～0x080FFFFF）+ Bank 2（0x08100000～0x081FFFFF），每个扇区 8 KB，
 *   按 16 字节（quad-word）写，写之前必须擦（每 16 字节带 ECC，同一位置不能写两次）
 *
 *   区域          起始          大小        Bank
 *   Bootloader    0x08000000    256 KB      1        第 7b 步
 *   App           0x08040000    到点表区为止 1 → 2    第 7b 步起 App 挪到这里（7a 还在 0x08000000）
 *   点表区        0x081FC000    8 KB        2        下发的点表，重启后还在（7a）
 *   配置区        0x081FE000    8 KB        2        Bootloader 的启动判断用（7a 写，7b 用）
 *
 * 点表区和配置区放在 Bank 2 末尾：App 在 Bank 1 运行时擦写 Bank 2，CPU 不用停下来等（读写不同 Bank 可以同时进行）。
 * 选项字节已确认（10/26 用 Keil 读寄存器）：TrustZone 关闭、两个 Bank 没交换、没有写保护和隐藏保护区。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#define FL_BASE             0x08000000u
#define FL_SIZE             0x00200000u     /* 2 MB */
#define FL_BANK_SIZE        0x00100000u     /* 1 MB */
#define FL_SECTOR_SIZE      0x2000u         /* 8 KB */
#define FL_WRITE_UNIT       16u             /* 一次写 16 字节 */

#define FL_BOOT_ADDR        0x08000000u
#define FL_BOOT_SIZE        0x00040000u     /* 256 KB */
#define FL_APP_ADDR         0x08040000u
#define FL_PT_ADDR          0x081FC000u     /* 点表区 */
#define FL_CFG_ADDR         0x081FE000u     /* 配置区 */
#define FL_APP_MAX          (FL_PT_ADDR - FL_APP_ADDR)

#define FL_RAM_START        0x20000000u     /* 检查 App 的初始栈指针用：H563 SRAM 640 KB */
#define FL_RAM_END          0x200A0000u

#endif /* FLASH_LAYOUT_H */
