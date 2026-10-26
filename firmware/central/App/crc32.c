/*
 * crc32.c - CRC-32，按位算（不用 1 KB 的查找表：点表最多 1 KB，速度够用）
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
