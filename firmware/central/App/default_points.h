/*
 * default_points.h - 内置的默认点表（第 6 步之前，点表只能编译进固件；第 6 步起可以由上位机下发）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef DEFAULT_POINTS_H
#define DEFAULT_POINTS_H

#include <stdint.h>

/* 把默认点表编码成点表文件格式写进 buf，返回字节数（空间不够返回 0）。
 * 故意走一遍"编码 → 解析"：和以后从上位机收到文件的路径完全一样 */
uint32_t default_points(uint8_t *buf, uint32_t size);

#endif /* DEFAULT_POINTS_H */
