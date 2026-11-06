// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef PF_TEST_PICO_STDIO_UART_H
#define PF_TEST_PICO_STDIO_UART_H

#include <stdint.h>

#include "hardware/uart.h"
#include "pico/stdio.h"

extern stdio_driver_t stdio_uart;

void stdio_uart_init_full(uart_inst_t *uart, uint32_t baud_rate, int tx_gpio, int rx_gpio);

#endif
