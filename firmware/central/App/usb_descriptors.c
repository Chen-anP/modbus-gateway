/*
 * usb_descriptors.c - USB 描述符：电脑插上中控后，靠这些"自我介绍"知道它是一个虚拟串口
 *
 * 枚举过程（电脑做的，TinyUSB 在 usbd 任务里应答）：
 *   1. 设备描述符     我是谁：VID / PID、用了 IAD（一个设备里有组合接口）
 *   2. 配置描述符     我有哪些接口和端点：CDC 控制接口（通知端点 0x81）+ CDC 数据接口（OUT 0x02 / IN 0x82）
 *   3. 字符串描述符   厂商、产品名、序列号（设备管理器里看到的名字）
 * Windows 10 以后看到 CDC ACM 类会自动装驱动（usbser.sys），出现一个 COM 口。
 *
 * 结构参考 TinyUSB 0.20.0 的例子 examples/device/cdc_msc/src/usb_descriptors.c（MIT），只保留 CDC。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "stm32h5xx.h"      /* UID_BASE：芯片唯一 ID，用作序列号 */
#include "tusb.h"

/* VID 0xCAFE 是 TinyUSB 例子用的开发用 ID，只能自己调试用；做成产品要向 USB-IF 申请或用芯片厂商分配的 PID */
#define USB_VID     0xCAFEu
#define USB_PID     0x4011u     /* 和 TinyUSB 例子的 PID 错开，避免电脑缓存的驱动配置冲突 */

/* ---------------------------------------------------------------- 设备描述符 */

static const tusb_desc_device_t k_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,          /* CDC 有两个接口，用 IAD 把它们绑成一个功能， */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,     /* 规范要求这时设备类填 MISC / COMMON / IAD     */
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,                   /* 设备版本 1.00 */
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&k_desc_device;
}

/* ---------------------------------------------------------------- 配置描述符 */

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF     0x81    /* 中断 IN：通知（串口状态） */
#define EPNUM_CDC_OUT       0x02    /* 批量 OUT：电脑 → 中控 */
#define EPNUM_CDC_IN        0x82    /* 批量 IN：中控 → 电脑 */
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t k_desc_config[] = {
    /* 配置号、接口数、字符串、总长度、属性、最大电流 mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* 接口号、字符串、通知端点及大小、数据端点 OUT / IN 及大小 */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 16, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return k_desc_config;
}

/* ---------------------------------------------------------------- 字符串描述符 */

static const char *const k_strings[] = {
    NULL,                       /* 0：语言（下面单独处理，0x0409 英语） */
    "chenpan",                  /* 1：厂商 */
    "Modbus Gateway",           /* 2：产品 */
    NULL,                       /* 3：序列号，用芯片唯一 ID 生成 */
    "Modbus Gateway CDC",       /* 4：CDC 接口 */
};

static uint16_t s_desc_str[1 + 32];     /* UTF-16：第 0 个元素是"长度 + 类型"，后面每个字符 2 字节 */

/* 芯片唯一 ID 的前 64 位转成 16 个十六进制字符 */
static size_t serial_string(uint16_t *out)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint32_t uid[2] = { *(const volatile uint32_t *)UID_BASE, *(const volatile uint32_t *)(UID_BASE + 4u) };
    size_t n = 0;

    for (size_t w = 0; w < 2; w++)
        for (int shift = 28; shift >= 0; shift -= 4)
            out[n++] = (uint16_t)hex[(uid[w] >> shift) & 0xFu];
    return n;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    size_t n = 0;
    (void)langid;

    if (index == 0) {
        s_desc_str[1] = 0x0409;
        n = 1;
    } else if (index == 3) {
        n = serial_string(&s_desc_str[1]);
    } else if (index < sizeof(k_strings) / sizeof(k_strings[0])) {
        const char *s = k_strings[index];
        n = strlen(s);
        if (n > 32)
            n = 32;
        for (size_t i = 0; i < n; i++)
            s_desc_str[1 + i] = (uint16_t)s[i];     /* 都是 ASCII，直接扩成 16 位 */
    } else {
        return NULL;                                /* 没有这个字符串：TinyUSB 会回 STALL */
    }
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
    return s_desc_str;
}
