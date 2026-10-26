/*
 * crc32.h - CRC-32（IEEE 802.3，和 zip、PNG、Python 的 zlib.crc32 相同）
 *
 *   多项式 0x04C11DB7（反射形式 0xEDB88320），初值 0xFFFFFFFF，结果取反
 *   "123456789" 的 CRC-32 = 0xCBF43926（标准校验值，test_file_rx.c 里测）
 *
 * 可以分段算：crc = crc32_update(0, a, n1); crc = crc32_update(crc, b, n2); 结果和一次算完一样。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#endif /* CRC32_H */
