/*
 * flash_if.c - Flash 擦写，说明见 flash_if.h
 *
 * STM32H563 的 Flash 要点：
 *   - 两个 Bank 各 1 MB，扇区 8 KB；擦除用 HAL_FLASHEx_Erase(Bank, 扇区号)，扇区号在每个 Bank 里从 0 数起
 *   - 编程单位是 16 字节（quad-word），每 16 字节有 ECC，所以同一个 16 字节擦一次只能写一次
 *   - 擦写前 HAL_FLASH_Unlock()，完了 HAL_FLASH_Lock()
 *   - ICACHE 会缓存 Flash 里读过的内容：擦写前关掉，擦写后打开（重新打开前会失效全部缓存行），
 *     否则刚写进去的东西可能读到旧的
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "main.h"
#include "log.h"
#include "flash_layout.h"
#include "flash_if.h"

static uint32_t         s_win_start, s_win_end;
static flash_if_stats_t s_st;

void flash_if_set_window(uint32_t start, uint32_t end)
{
    s_win_start = start;
    s_win_end   = end;
}

static int in_window(uint32_t addr, uint32_t len)
{
    return addr >= s_win_start && len <= s_win_end - s_win_start && addr - s_win_start <= s_win_end - s_win_start - len;
}

/* 擦写前后：关 / 开 ICACHE，解锁 / 上锁 */
static uint32_t begin(void)
{
    uint32_t ic = HAL_ICACHE_IsEnabled();
    if (ic)
        (void)HAL_ICACHE_Disable();
    (void)HAL_FLASH_Unlock();
    return ic;
}

static void end(uint32_t ic)
{
    (void)HAL_FLASH_Lock();
    if (ic) {
        (void)HAL_ICACHE_Invalidate();
        (void)HAL_ICACHE_Enable();
    }
}

static int fi_erase(uint32_t addr)
{
    FLASH_EraseInitTypeDef e;
    uint32_t sector_err = 0, ic, t0;
    HAL_StatusTypeDef r;

    if (!in_window(addr, FL_SECTOR_SIZE) || (addr - FL_BASE) % FL_SECTOR_SIZE != 0) {
        s_st.refused++;
        return -1;
    }
    memset(&e, 0, sizeof(e));
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks     = (addr >= FL_BASE + FL_BANK_SIZE) ? FLASH_BANK_2 : FLASH_BANK_1;
    e.Sector    = ((addr - FL_BASE) % FL_BANK_SIZE) / FL_SECTOR_SIZE;
    e.NbSectors = 1;

    t0 = HAL_GetTick();
    ic = begin();
    r = HAL_FLASHEx_Erase(&e, &sector_err);
    if (r != HAL_OK)
        s_st.last_hal_error = HAL_FLASH_GetError();
    end(ic);
    s_st.last_erase_ms = HAL_GetTick() - t0;
    if (r != HAL_OK) {
        s_st.hal_errors++;
        return -2;
    }
    s_st.erases++;
    return 0;
}

static int fi_write(uint32_t addr, const uint8_t *d, uint32_t len)
{
    uint32_t q[4];              /* HAL 按 32 位读源数据：先复制到对齐的缓冲区 */
    uint32_t off, ic;
    int rc = 0;

    if (!in_window(addr, len) || (addr & 15u) != 0 || (len & 15u) != 0) {
        s_st.refused++;
        return -1;
    }
    ic = begin();
    for (off = 0; off < len; off += 16) {
        memcpy(q, &d[off], 16);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, addr + off, (uint32_t)q) != HAL_OK) {
            s_st.last_hal_error = HAL_FLASH_GetError();
            s_st.hal_errors++;
            rc = -2;
            break;
        }
        s_st.writes++;
    }
    end(ic);
    return rc;
}

static const uint8_t *fi_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

static const flash_io_t s_io = { fi_erase, fi_write, fi_ptr };

const flash_io_t       *flash_if_io(void)    { return &s_io; }
const flash_if_stats_t *flash_if_stats(void) { return &s_st; }

void flash_if_log_option_bytes(void)
{
    uint32_t optsr  = FLASH->OPTSR_CUR;
    uint32_t optsr2 = FLASH->OPTSR2_CUR;

    /* PRODUCT_STATE 0xED = Open；SWAP_BANK 0 = 没交换；TZEN 0xC3 = TrustZone 关 */
    LOGI("flash", "option bytes: product state 0x%02X, swap bank %u, TZEN 0x%02X (OPTSR 0x%08lX)",
         (unsigned)((optsr >> 8) & 0xFFu), (unsigned)((optsr >> 31) & 1u), (unsigned)((optsr2 >> 24) & 0xFFu),
         (unsigned long)optsr);
}
