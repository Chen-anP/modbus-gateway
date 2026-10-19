#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, void **handle);
#ifndef STUB_TASK_EXTRA
#define STUB_TASK_EXTRA
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t t);
void vTaskDelete(void *h);
#endif
