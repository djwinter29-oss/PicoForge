#ifndef PF_TEST_SDIO_SDK_STUBS_H
#define PF_TEST_SDIO_SDK_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "sdio_pio_host.h"

#define PF_TEST_SDIO_MAX_RX_BATCH_BYTES (64u * 520u)

extern pf_sdio_pio_host_t *pf_test_sdio_observed_host;
extern bool pf_test_sdio_expect_streams_cleared;
extern bool pf_test_sdio_streams_were_cleared;
extern bool pf_test_sdio_can_add_program;
extern unsigned pf_test_sdio_removed_programs;
extern dma_hw_t pf_test_sdio_dma_registers;
extern unsigned pf_test_sdio_barrier_count;
extern bool pf_test_sdio_data_irq;
extern bool pf_test_sdio_dma_busy;
extern bool pf_test_sdio_corrupt_command_crc;
extern unsigned pf_test_sdio_data_fifo_words;
extern uint32_t pf_test_sdio_data_response;
extern uint32_t pf_test_sdio_time_step;
extern unsigned pf_test_sdio_command_count;
extern uint8_t pf_test_sdio_last_command;
extern uint32_t pf_test_sdio_dma_word_count;
extern dma_channel_config pf_test_sdio_last_dma_config;
extern uint32_t pf_test_sdio_gpio_level;
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
