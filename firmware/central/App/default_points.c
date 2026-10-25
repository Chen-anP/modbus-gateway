/*
 * default_points.c - 内置默认点表：课程的三块传感器（寄存器表见学习资料《传感器寄存器表》）
 *
 * 中控地址的分配规则：每块传感器在每张表里占 10 个地址，按设备地址排：
 *   开关量（ch1，地址 1）     0～9
 *   环境监测（ch1，地址 2）   10～19
 *   温湿度（ch2，地址 3）     20～29
 *
 *   中控            ←→  传感器                       内容
 *   0x  0～4            ch1 地址 1  0x 0～4           开关量板：输出 1、2，LED1～3
 *   1x  0～2            ch1 地址 1  1x 0～2           开关量板：按键 1～3
 *   0x 10～14           ch1 地址 2  0x 0～4           环境监测板：蜂鸣器 1、2（会响！），LED1～3
 *   3x 10～11           ch1 地址 2  3x 0～1           环境监测板：ADC 两路
 *   0x 20～24           ch2 地址 3  0x 0～4           温湿度板：蜂鸣器 1、2，LED1～3
 *   3x 20～21           ch2 地址 3  3x 0～1           温湿度板：温度（0.1 ℃）、湿度（0.1 %）
 *
 * 不映射传感器的保持寄存器 0：那是升级命令寄存器（写 0x55 会让传感器进 Bootloader），
 * 升级走系统区的转发命令（第 8 步），不放进点表，免得被误写。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "mb_rtu.h"
#include "point_table.h"
#include "default_points.h"

#define P(tab, ch, dev, gw, da)  { (uint8_t)(tab), (uint8_t)(ch), (uint8_t)(dev), (uint16_t)(gw), (uint16_t)(da) }

static const pt_point_t k_points[] = {
    /* 开关量板：ch1 地址 1 */
    P(MB_TAB_COIL,     1, 1,  0, 0), P(MB_TAB_COIL, 1, 1,  1, 1), P(MB_TAB_COIL, 1, 1,  2, 2),
    P(MB_TAB_COIL,     1, 1,  3, 3), P(MB_TAB_COIL, 1, 1,  4, 4),
    P(MB_TAB_DISCRETE, 1, 1,  0, 0), P(MB_TAB_DISCRETE, 1, 1, 1, 1), P(MB_TAB_DISCRETE, 1, 1, 2, 2),
    /* 环境监测板：ch1 地址 2 */
    P(MB_TAB_COIL,     1, 2, 10, 0), P(MB_TAB_COIL, 1, 2, 11, 1), P(MB_TAB_COIL, 1, 2, 12, 2),
    P(MB_TAB_COIL,     1, 2, 13, 3), P(MB_TAB_COIL, 1, 2, 14, 4),
    P(MB_TAB_INPUT,    1, 2, 10, 0), P(MB_TAB_INPUT, 1, 2, 11, 1),
    /* 温湿度板：ch2 地址 3 */
    P(MB_TAB_COIL,     2, 3, 20, 0), P(MB_TAB_COIL, 2, 3, 21, 1), P(MB_TAB_COIL, 2, 3, 22, 2),
    P(MB_TAB_COIL,     2, 3, 23, 3), P(MB_TAB_COIL, 2, 3, 24, 4),
    P(MB_TAB_INPUT,    2, 3, 20, 0), P(MB_TAB_INPUT, 2, 3, 21, 1),
};

uint32_t default_points(uint8_t *buf, uint32_t size)
{
    return point_table_encode(k_points, (uint16_t)(sizeof(k_points) / sizeof(k_points[0])), buf, size);
}
