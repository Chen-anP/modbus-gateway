/*
 * log.h - 带等级的日志模块，输出到 USART3（PC10 TX，115200）
 *
 * 用法：
 *   LOGI("boot", "clock %lu Hz", HAL_RCC_GetSysClockFreq());
 *   LOGW("ch1", "dev %u timeout", addr);
 *
 * 输出格式：[  12345] I boot: clock 250000000 Hz
 *            毫秒时间戳  等级 标签   内容
 *
 * 设计要点（详见学习笔记）：
 *   1. 调用 LOGx 的任务只负责格式化、放进队列，立即返回，不等串口发完；
 *      真正的发送由一个低优先级的日志任务完成，所以打日志不会拖慢通信任务。
 *   2. 队列满了就丢弃这一条并计数，绝不阻塞调用者。
 *   3. 调度器启动前（main 里初始化阶段）没有任务可用，直接阻塞发送。
 *   4. 中断里不能打日志（格式化太慢，而且会和任务抢串口），只计数。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

typedef enum {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_INFO  = 2,
    LOG_LVL_DEBUG = 3
} log_level_t;

/* 创建日志队列和日志任务。在 MX_FREERTOS_Init() 里调用（调度器启动前）。 */
void log_init(void);

/* 运行时修改输出等级：比它详细的日志不输出。默认 LOG_LVL_INFO。 */
void log_set_level(log_level_t level);

/* 一般不直接调用，用下面的宏 */
void log_write(log_level_t level, const char *tag, const char *fmt, ...);

/* 统计：因为队列满被丢掉的条数、在中断里被拒绝的条数 */
uint32_t log_dropped(void);
uint32_t log_isr_rejected(void);

/* ---- 练习：第 1 步你自己实现（见学习笔记"动手改"） ----
 * 以十六进制打印一段数据，每行最多 16 字节，例如：
 *   [  20001] D ch1: tx 8 bytes: 01 03 00 00 00 0A C5 CD
 * 第 2 步调试串口收发时要用它看每一帧的内容。 */
void log_hex(log_level_t level, const char *tag, const char *title, const uint8_t *data, uint16_t len);

#define LOGE(tag, ...)  log_write(LOG_LVL_ERROR, (tag), __VA_ARGS__)
#define LOGW(tag, ...)  log_write(LOG_LVL_WARN,  (tag), __VA_ARGS__)
#define LOGI(tag, ...)  log_write(LOG_LVL_INFO,  (tag), __VA_ARGS__)
#define LOGD(tag, ...)  log_write(LOG_LVL_DEBUG, (tag), __VA_ARGS__)

#endif /* LOG_H */
