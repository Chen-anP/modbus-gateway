/*
 * boot_usb.c - Bootloader 的 USB 虚拟串口：和 App 的 port_usb.c 做同样的事，但没有 RTOS
 *
 *   App：usbd 任务阻塞在 TinyUSB 的事件队列上；up 任务等信号量
 *   这里：主循环不停地 boot_usb_poll()（= tud_task()），收数据用查询
 *
 * USB 外设的时钟配置照抄 CubeMX 给 App 生成的 HAL_PCD_MspInit（HSI48 + CRS，见 SystemClock_Config）。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "main.h"
#include "tusb.h"
#include "log.h"
#include "boot_usb.h"

static volatile uint8_t s_ready;

/* TinyUSB 裸机需要的毫秒时间 */
uint32_t tusb_time_millis_api(void)
{
    return HAL_GetTick();
}

int boot_usb_init(void)
{
    RCC_PeriphCLKInitTypeDef clk = { 0 };
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    const tud_cdc_configure_t cdc_cfg = {
        .rx_persistent = false,
        .tx_persistent = false,
        .tx_overwritabe_if_not_connected = false,   /* 回应一个字节都不能丢（和 App 一样） */
    };

    clk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    clk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
        return -1;
    HAL_PWREx_EnableVddUSB();
    __HAL_RCC_USB_CLK_ENABLE();
    HAL_NVIC_SetPriority(USB_DRD_FS_IRQn, 5, 0);   /* 中断由 TinyUSB 在 tusb_init 里打开 */

    tud_cdc_configure(&cdc_cfg);
    if (!tusb_init(0, &init))
        return -2;
    s_ready = 1;
    return 0;
}

void boot_usb_irq(void)
{
    if (s_ready)
        tud_int_handler(0);
}

void boot_usb_poll(void)
{
    tud_task();
}

uint16_t boot_usb_read(uint8_t *buf, uint16_t max)
{
    if (tud_cdc_available() == 0)
        return 0;
    return (uint16_t)tud_cdc_read(buf, max);
}

int boot_usb_write(const uint8_t *d, uint16_t len, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint16_t done = 0;

    while (done < len) {
        done = (uint16_t)(done + tud_cdc_write(&d[done], (uint32_t)(len - done)));
        tud_cdc_write_flush();
        if (done < len) {
            if (!tud_mounted() || HAL_GetTick() - t0 >= timeout_ms)
                return -1;
            tud_task();                             /* FIFO 满了：处理 USB 事件，等电脑取走 */
        }
    }
    return 0;
}

void boot_wait_ms(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < ms)
        tud_task();
}

/* ---------------------------------------------------------------- TinyUSB 回调（在 tud_task 里执行） */

void tud_mount_cb(void)
{
    LOGI("usb", "mounted (enumerated by host)");
}

void tud_umount_cb(void)
{
    LOGI("usb", "unmounted");
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    LOGI("usb", "line state: DTR %u RTS %u", (unsigned)dtr, (unsigned)rts);
}
