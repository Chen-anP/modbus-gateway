/*
 * boot_main.c - 中控的 Bootloader（0x08000000，256 KB）
 *
 * 开机：
 *   1. 什么都不初始化，先读配置区、做判断（boot_cfg_decide）
 *   2. 该跳 → 日志串口打一行 "[boot] jump to app"（寄存器直接发，发完复位 USART3）→ 跳到 App（0x08040000）。
 *      这时时钟、外设、中断都还是复位后的样子，App 就像刚上电一样启动
 *   3. 该留下 → 初始化时钟、日志串口、USB，当一个 Modbus 从机（地址 1），等上位机发固件
 *
 * 留下以后：
 *   上位机 0x15 file_no 0x1000 发固件 → 整个收进 RAM（最大 512 KB）→ CRC、地址、向量表都对
 *   → 回应发出去以后写 Flash（app_flash_program）→ UPG_STATE = 3
 *   → 上位机写 CMD 0x0A00 → 清掉配置区的 stay → 复位 → 第 1 步判断通过 → 跳到新 App
 *
 * 用 Keil 直接下载 App 以后，配置区里还是上次升级记的长度和 CRC，对不上 → 留在 Bootloader（bad crc）。
 * 这时写 CMD 0x0A01：清掉配置区里的 App 信息，复位后只检查向量表就跳。
 *
 * 为什么用复位来"启动 App"，而不是直接跳：留在 Bootloader 时已经开了时钟、USB、SysTick 和中断，
 * 直接跳过去 App 会在一个"不干净"的状态下启动。复位以后在第 2 步跳，什么都没开，最干净。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "main.h"
#include "log.h"
#include "mb_rtu.h"
#include "flash_layout.h"
#include "flash_if.h"
#include "boot_cfg.h"
#include "app_flash.h"
#include "boot_proto.h"
#include "boot_usb.h"
#include "boot_version.h"
#include "boot_guard.h"
#include "guard_hw.h"

#define SLAVE_ADDR      1u
#define FRAME_GAP_MS    50u
#define FW_BUF_SIZE     (512u * 1024u)      /* 固件先整个收进 RAM：App 不能超过 512 KB */

UART_HandleTypeDef huart3;

static uint8_t        s_fw[FW_BUF_SIZE] __attribute__((aligned(4)));
static boot_proto_t   s_bp;
static mb_slave_ops_t s_ops;

static void SystemClock_Config(void);
static void uart3_init(void);

/* ---------------------------------------------------------------- 跳到 App */

/* 跳之前要保证：中断全关、没有挂起的中断、SysTick 停了、向量表指向 App。
 * 最后两步（换栈指针、跳到复位入口）必须连在一起用汇编写：换了栈以后不能再用任何 C 局部变量 */
static void jump_to_app(uint32_t addr)
{
    const volatile uint32_t *vec = (const volatile uint32_t *)addr;
    uint32_t sp = vec[0], entry = vec[1];
    uint32_t i;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    for (i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;        /* 关掉所有外设中断 */
        NVIC->ICPR[i] = 0xFFFFFFFFu;        /* 清掉挂起的 */
    }
    SCB->VTOR = addr;                       /* App 的中断向量表 */
    __set_MSPLIM(0);                        /* 栈下限检查交给 App 自己设 */
    __DSB();
    __ISB();
    __enable_irq();                         /* 复位后 PRIMASK = 0，App 也这样期待 */
    __asm volatile ("msr msp, %0\n\tbx %1" : : "r"(sp), "r"(entry) : "memory");
    for (;;) { }
}

/* 跳之前在日志串口打一行（不用 HAL、不用中断）：
 * 不打的话，跳走以后串口一个字都没有，分不清"Bootloader 跳了"还是"Bootloader 没跑起来"（调试记录第 9 条）。
 * 用寄存器直接配 USART3，发完把 USART3 和 GPIOC 复位、关时钟，App 看到的还是复位后的样子。
 * 这时 CPU 跑在复位后的 HSI 上，波特率按 SystemCoreClockUpdate() 算出的时钟设。 */
static void early_print(const char *s)
{
    uint32_t pclk;

    SystemCoreClockUpdate();
    pclk = HAL_RCC_GetPCLK1Freq();          /* 只读寄存器、算频率，不需要 HAL_Init */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();
    GPIOC->AFR[1] = (GPIOC->AFR[1] & ~(0xFu << 8)) | (7u << 8);     /* PC10 = AF7（USART3_TX） */
    GPIOC->MODER  = (GPIOC->MODER & ~(3u << 20)) | (2u << 20);      /* PC10 复用功能 */
    USART3->BRR = (pclk + 115200u / 2u) / 115200u;
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
    while (*s) {
        while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0) { }
        USART3->TDR = (uint8_t)*s++;
    }
    while ((USART3->ISR & USART_ISR_TC) == 0) { }                   /* 最后一个字节发完 */

    __HAL_RCC_USART3_FORCE_RESET();         /* 恢复成复位后的样子 */
    __HAL_RCC_USART3_RELEASE_RESET();
    __HAL_RCC_GPIOC_FORCE_RESET();
    __HAL_RCC_GPIOC_RELEASE_RESET();
    __HAL_RCC_USART3_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
}

/* ---------------------------------------------------------------- 主循环要做的事 */

static void do_program(void)
{
    const flash_if_stats_t *fs = flash_if_stats();
    uint32_t t0 = HAL_GetTick();
    af_err_t e;

    LOGI("boot", "programming %lu bytes, crc %08lX, ver %lu ...", (unsigned long)s_bp.rx.hdr.length,
         (unsigned long)s_bp.rx.hdr.crc32, (unsigned long)s_bp.rx.hdr.version);
    e = app_flash_program(flash_if_io(), &s_bp.layout, s_bp.rx.buf, s_bp.rx.hdr.length,
                          s_bp.rx.hdr.crc32, s_bp.rx.hdr.version);
    boot_proto_program_done(&s_bp, e);
    if (e == AF_OK)
        (void)bkp_write_check(BKP_REG_TRIES, bg_tries_encode(0));   /* 新 App 重新有 3 次机会 */
    if (e == AF_OK)
        LOGI("boot", "programmed in %lu ms (%lu sectors erased, %lu x 16 bytes written); send 0x0A00 to start it",
             (unsigned long)(HAL_GetTick() - t0), (unsigned long)fs->erases, (unsigned long)fs->writes);
    else
        LOGE("boot", "program FAILED: %s (hal errors %lu, last 0x%08lX, refused %lu)", af_err_name(e),
             (unsigned long)fs->hal_errors, (unsigned long)fs->last_hal_error, (unsigned long)fs->refused);
}

/* forget = 0：清 stay（0x0A00）；forget = 1：再把 App 长度和 CRC 清掉（0x0A01），开机只看向量表 */
static void do_run_app(int forget)
{
    boot_cfg_t c;

    if (boot_cfg_read(flash_if_io(), FL_CFG_ADDR, &c) && (c.stay || (forget && c.app_len))) {
        c.stay = 0;
        if (forget) {
            c.app_len = 0;
            c.app_crc = 0;
        }
        if (boot_cfg_write(flash_if_io(), FL_CFG_ADDR, &c) != 0) {
            LOGE("boot", "config write FAILED, stay in bootloader");
            return;
        }
    }
    (void)bkp_write_check(BKP_REG_TRIES, bg_tries_encode(0));      /* 人工要求启动：重新给 3 次机会 */
    LOGW("boot", "start app%s: reset", forget ? " (app info cleared)" : "");
    boot_wait_ms(100);                      /* 等回应送到电脑、日志打完 */
    NVIC_SystemReset();
}

static void handle_frame(const uint8_t *req, uint16_t len)
{
    uint8_t rsp[MB_ADU_MAX];
    uint16_t n = mb_slave_handle(SLAVE_ADDR, &s_ops, req, len, rsp);

    if (n > 0 && boot_usb_write(rsp, n, 500) != 0)
        LOGW("boot", "usb write timeout");

    switch (boot_proto_take_action(&s_bp)) {    /* 回应发出去以后再做 */
    case BP_ACT_PROGRAM: do_program();                                  break;
    case BP_ACT_RUN_APP:    do_run_app(0);                              break;
    case BP_ACT_FORGET_APP: do_run_app(1);                              break;
    case BP_ACT_REBOOT:  LOGW("boot", "reboot"); boot_wait_ms(100); NVIC_SystemReset(); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    static const af_layout_t layout = { FL_APP_ADDR, FL_APP_MAX, FL_CFG_ADDR, FL_RAM_START, FL_RAM_END };
    boot_cfg_t cfg;
    boot_decision_t d;
    int has_cfg;
    mb_framer_t f;
    uint8_t chunk[64];

    /* 1. 判断（只读 Flash，什么都没初始化） */
    has_cfg = boot_cfg_read(flash_if_io(), FL_CFG_ADDR, &cfg);
    d = boot_cfg_decide(flash_if_io(), has_cfg ? &cfg : NULL, FL_APP_ADDR, FL_APP_MAX, FL_RAM_START, FL_RAM_END);

    /* 2. 跳 */
    if (d == BOOT_JUMP || d == BOOT_JUMP_NO_CFG) {
        uint32_t tries;

        bkp_open();
        tries = bg_tries(bkp_read(BKP_REG_TRIES));
        if (bg_too_many(tries)) {
            d = BOOT_STAY_BOOTLOOP;             /* 连续几次都没确认：不跳了（第 8 步） */
            (void)bkp_write_check(BKP_REG_FLAGS, bg_flags_encode(0));   /* 清掉"开机就死机"测试标志，下次 run-app 就正常了 */
        } else {
            static char msg[] = "\r\n[boot] jump to app, try ?/3\r\n";
            static char msg_nc[] = "\r\n[boot] jump to app (no config, crc not checked), try ?/3\r\n";
            char *m = (d == BOOT_JUMP) ? msg : msg_nc;

            int ok = bkp_write_check(BKP_REG_TRIES, bg_tries_encode(tries + 1u));   /* App 稳定 5 秒后清零 */
            /* 不调用 bkp_close()：保持备份域可写、时钟开着，App 反正也要打开（10/26 写了马上关，值丢了） */
            m[strlen(m) - 5] = (char)('1' + tries);                  /* "?" → 第几次 */
            early_print(m);
            if (!ok)
                early_print("[boot] WARNING: backup register write failed, boot tries not counted\r\n");
            iwdg_start();                       /* App 启动过程中卡死也会被复位；App 里的看门狗任务接着喂 */
            jump_to_app(FL_APP_ADDR);
        }
    }

    /* 3. 留下 */
    HAL_Init();
    SystemClock_Config();
    uart3_init();
    log_init();
    LOGI("boot", "bootloader 0x%04X, sysclk %lu Hz", (unsigned)BOOT_VERSION, (unsigned long)HAL_RCC_GetSysClockFreq());
    LOGW("boot", "stay in bootloader: %s", boot_decision_name(d));
    {
        uint32_t rsr = RCC->RSR;
        bkp_open();
        LOGI("boot", "reset reason: %s (RSR 0x%08lX), boot tries %lu/%u", rr_name(rr_decode(rsr)),
             (unsigned long)rsr, (unsigned long)bg_tries(bkp_read(BKP_REG_TRIES)), (unsigned)BG_MAX_TRIES);
        RCC->RSR |= RCC_RSR_RMVF;               /* 留下时清掉：App 下次读到的只是最后那一次复位的原因 */
    }
    if (has_cfg)
        LOGI("boot", "config: app len %lu crc %08lX ver %lu, stay %lu", (unsigned long)cfg.app_len,
             (unsigned long)cfg.app_crc, (unsigned long)cfg.app_ver, (unsigned long)cfg.stay);
    else
        LOGI("boot", "config: none");

    flash_if_set_window(FL_APP_ADDR, FL_BASE + FL_SIZE);  /* Bootloader 自己（前 256 KB）不能被擦 */
    boot_proto_init(&s_bp, s_fw, sizeof(s_fw), &layout, BOOT_VERSION, (uint16_t)d,
                    has_cfg ? (uint16_t)cfg.app_ver : 0u);
    boot_proto_ops(&s_bp, &s_ops);

    if (boot_usb_init() != 0)
        LOGE("boot", "usb init failed");
    else
        LOGI("usb", "TinyUSB started (no RTOS), slave addr %u", (unsigned)SLAVE_ADDR);

    mb_framer_init(&f, 1, FRAME_GAP_MS);
    for (;;) {
        uint16_t n, used;
        const uint8_t *p;

        boot_usb_poll();
        n = boot_usb_read(chunk, sizeof(chunk));
        p = chunk;
        while (n > 0) {
            if (mb_framer_feed(&f, p, n, HAL_GetTick(), &used)) {
                handle_frame(f.buf, f.len);
                mb_framer_reset(&f);
            }
            p += used;
            n = (uint16_t)(n - used);
        }
    }
}

/* ---------------------------------------------------------------- 硬件初始化（照抄 App 的 CubeMX 生成代码） */

/* 250 MHz：CSI → PLL1；USB 用 HSI48，CRS 按 USB 的 SOF 校准。和 App 的 SystemClock_Config 一样 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };
    RCC_CRSInitTypeDef crs = { 0 };

    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_CSI;
    osc.HSI48State          = RCC_HSI48_ON;
    osc.CSIState            = RCC_CSI_ON;
    osc.CSICalibrationValue = RCC_CSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLL1_SOURCE_CSI;
    osc.PLL.PLLM            = 1;
    osc.PLL.PLLN            = 125;
    osc.PLL.PLLP            = 2;
    osc.PLL.PLLQ            = 2;
    osc.PLL.PLLR            = 2;
    osc.PLL.PLLRGE          = RCC_PLL1_VCIRANGE_2;
    osc.PLL.PLLVCOSEL       = RCC_PLL1_VCORANGE_WIDE;
    osc.PLL.PLLFRACN        = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
        Error_Handler();

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1
                       | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_PCLK3;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    clk.APB3CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
        Error_Handler();

    __HAL_RCC_CRS_CLK_ENABLE();
    crs.Prescaler             = RCC_CRS_SYNC_DIV1;
    crs.Source                = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue       = 34;
    crs.HSI48CalibrationValue = 32;
    HAL_RCCEx_CRSConfig(&crs);

    __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);   /* 250 MHz 下写 Flash 必须设 */
}

/* USART3（PC10 TX / PC11 RX），115200 8N1，只用来打日志 */
static void uart3_init(void)
{
    huart3.Instance            = USART3;
    huart3.Init.BaudRate       = 115200;
    huart3.Init.WordLength     = UART_WORDLENGTH_8B;
    huart3.Init.StopBits       = UART_STOPBITS_1;
    huart3.Init.Parity         = UART_PARITY_NONE;
    huart3.Init.Mode           = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling   = UART_OVERSAMPLING_16;
    huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart3) != HAL_OK)
        Error_Handler();
}

void HAL_UART_MspInit(UART_HandleTypeDef *h)
{
    RCC_PeriphCLKInitTypeDef clk = { 0 };
    GPIO_InitTypeDef gpio = { 0 };

    if (h->Instance != USART3)
        return;
    clk.PeriphClockSelection  = RCC_PERIPHCLK_USART3;
    clk.Usart3ClockSelection  = RCC_USART3CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
        Error_Handler();
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin       = GPIO_PIN_10 | GPIO_PIN_11;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOC, &gpio);
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) { }
}
