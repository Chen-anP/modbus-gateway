/* PC 测试用的最小 FreeRTOS 替身：只提供 log.c 用到的接口（名字和值与真正的 FreeRTOS 一致） */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS pdTRUE
#define pdFAIL pdFALSE
#define portMAX_DELAY 0xFFFFFFFFu
#define tskIDLE_PRIORITY 0
#define configASSERT(x) do { if (!(x)) { extern void stub_assert_fail(void); stub_assert_fail(); } } while (0)
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING 2
