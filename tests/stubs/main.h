#pragma once
#include <stdint.h>
typedef struct { int dummy; } UART_HandleTypeDef;
uint32_t HAL_GetTick(void);
int HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *d, uint16_t n, uint32_t t);
uint32_t __get_IPSR(void);
