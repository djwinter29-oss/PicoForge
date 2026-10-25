#ifndef PF_MONITOR_UART_H
#define PF_MONITOR_UART_H

#include <stdint.h>

/** Board-supplied monitor UART assignment. */
typedef struct {
    uint32_t uart_index; /**< Hardware UART index: 0 or 1. */
    uint32_t tx_gpio; /**< TX GPIO number. */
    uint32_t rx_gpio; /**< RX GPIO number. */
    uint32_t baud_rate; /**< Line rate in baud. */
} pf_monitor_uart_board_t;

/** Build-selected assignment, defined in config/board.c. */
extern const pf_monitor_uart_board_t pf_monitor_uart_board;

/**
 * @brief Route stdio exclusively to the pf_monitor_uart_board UART; later calls are no-ops.
 */
void pf_monitor_uart_init(void);

#endif
