/*
 * flash_io.h - 擦写 Flash 的一组函数指针
 *
 * 点表存储（pt_store）、配置区（boot_cfg）只通过这组函数访问 Flash，自己不碰硬件：
 *   板子上   flash_if.c 用 HAL 实现，地址就是真实的 Flash 地址
 *   PC 测试  tests/fake_flash.h 用一个数组模拟，还能模拟"写到一半断电"
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef FLASH_IO_H
#define FLASH_IO_H

#include <stdint.h>

typedef struct {
    int (*erase)(uint32_t sector_addr);                                 /* 擦一个 8 KB 扇区，成功 0 */
    int (*write)(uint32_t addr, const uint8_t *data, uint32_t len);     /* addr 16 字节对齐、len 是 16 的倍数，成功 0 */
    const uint8_t *(*ptr)(uint32_t addr);                               /* 读：返回能直接读的指针 */
} flash_io_t;

#endif /* FLASH_IO_H */
