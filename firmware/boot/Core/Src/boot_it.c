/*
 * boot_it.c - Bootloader 用到的中断：SysTick（HAL 的毫秒计时）、USB、出错
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include "main.h"
#include "boot_usb.h"

void NMI_Handler(void)
{
    for (;;) { }
}

void HardFault_Handler(void)
{
    for (;;) { }
}

void MemManage_Handler(void)  { for (;;) { } }
void BusFault_Handler(void)   { for (;;) { } }
void UsageFault_Handler(void) { for (;;) { } }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USB_DRD_FS_IRQHandler(void)
{
    boot_usb_irq();
}
