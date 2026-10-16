#ifndef PF_TEST_SDIO_SDK_STUBS_H
#define PF_TEST_SDIO_SDK_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "sdio_pio_host.h"

#define PF_TEST_SDIO_MAX_RX_BATCH_BYTES (64u * 520u)

typedef enum {
    PF_TEST_RX_DISABLE, PF_TEST_RX_CLEAR, PF_TEST_RX_IRQ_CLEAR,
    PF_TEST_RX_RESTART, PF_TEST_RX_EXEC, PF_TEST_RX_PUT,
    PF_TEST_DMA_CONFIGURE, PF_TEST_DMA_START, PF_TEST_RX_ENABLE,
    PF_TEST_DMA_ABORT, PF_TEST_CMD18, PF_TEST_CMD12, PF_TEST_BARRIER
} pf_test_sdio_event_t;
extern pf_test_sdio_event_t pf_test_sdio_events[256];
extern unsigned pf_test_sdio_event_count;
extern bool pf_test_sdio_trace_enabled;
extern void (*pf_test_sdio_dma_poll_hook)(void);

extern pf_sdio_pio_host_t *pf_test_sdio_observed_host;
extern bool pf_test_sdio_expect_streams_cleared;
extern bool pf_test_sdio_streams_were_cleared;
extern bool pf_test_sdio_can_add_program;
extern unsigned pf_test_sdio_removed_programs;
extern dma_hw_t pf_test_sdio_dma_registers;
extern unsigned pf_test_sdio_barrier_count;
extern bool pf_test_sdio_data_irq;
extern bool pf_test_sdio_dma_busy;
extern uint32_t pf_test_sdio_system_clock_hz;
extern bool pf_test_sdio_force_tx_stall;
extern bool pf_test_sdio_corrupt_command_crc;
extern unsigned pf_test_sdio_data_fifo_words;
extern uint32_t pf_test_sdio_data_response;
extern uint32_t pf_test_sdio_time_step;
extern unsigned pf_test_sdio_command_count;
extern uint8_t pf_test_sdio_last_command;
extern uint32_t pf_test_sdio_dma_word_count;
extern dma_channel_config pf_test_sdio_last_dma_config;
extern uint32_t pf_test_sdio_gpio_level;
extern uint32_t pf_test_sdio_clock_gpio;
extern bool pf_test_sdio_clock_auto_toggle;
extern unsigned pf_test_sdio_clock_rising_edges;
extern uint16_t pf_test_sdio_config_divider_integer;
extern uint8_t pf_test_sdio_config_divider_fraction;
extern uint32_t pf_test_sdio_clkdiv_restart_mask;
extern uint32_t pf_test_sdio_sync_mask;
extern unsigned pf_test_sdio_tx_preload_queries;
extern unsigned pf_test_sdio_dma_busy_delay;
extern unsigned pf_test_sdio_data_irq_delay;
extern unsigned pf_test_sdio_data_irq_queries;
extern unsigned pf_test_sdio_dma_busy_queries;
extern unsigned pf_test_sdio_dma_abort_count;
extern uint8_t pf_test_sdio_rx_wire[40];
extern size_t pf_test_sdio_rx_wire_size;
extern bool pf_test_sdio_data_wait_ok;
extern unsigned pf_test_sdio_command_fail;
extern unsigned pf_test_sdio_command_error;
extern unsigned pf_test_sdio_corrupt_command;
extern unsigned pf_test_sdio_pio_restart_calls;
extern uint32_t pf_test_sdio_pio_trace[32];
extern unsigned pf_test_sdio_pio_trace_count;
extern uint8_t pf_test_sdio_batch_rx[PF_TEST_SDIO_MAX_RX_BATCH_BYTES];
extern size_t pf_test_sdio_batch_rx_size;

#endif
