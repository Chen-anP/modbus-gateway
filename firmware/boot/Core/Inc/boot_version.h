/*
 * boot_version.h - Bootloader 的版本号（在 Bootloader 里读系统区 0xF002 得到）
 *
 * 和 App 的版本号分开：0x01xx = Bootloader 第 1 版。Bootloader 一般装好就不再升级。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef BOOT_VERSION_H
#define BOOT_VERSION_H

#define BOOT_VERSION    0x0101u     /* 0x0101：第 8 步，启动次数 + 看门狗 */

#endif /* BOOT_VERSION_H */
