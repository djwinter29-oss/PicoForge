#ifndef PF_SDIO_PIO_HOST_H
#define PF_SDIO_PIO_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdio_pio.h"
#include "sdio_pio_protocol.h"

typedef struct {
    PIO pio;
    uint command_gpio;
    uint data_gpio_base;
    uint data0_gpio;
    uint clock_gpio;
    uint command_sm;
    uint data_sm;
    uint command_program_offset;
    uint data_rx_program_offset;
    uint clock_program_offset;
    uint data_tx_program_offset;
    uint dma_channel;
    uint32_t system_clock_hz;
    uint32_t current_clock_hz;
    uint32_t previous_input_sync_bypass;
    uint32_t input_sync_bypass_mask;
    uint32_t last_response_words[5];
    size_t last_response_word_count;
    uint32_t write_stream_next_sector;
    bool write_stream_open;
    bool command_program_loaded;
    bool data_rx_program_loaded;
    bool data_rx_fast;
    bool clock_program_loaded;
    bool data_tx_program_loaded;
    bool data_tx_fast;
    uint32_t clock_div_ticks; /* Command/clock divider in 1/256 units. */
} pf_sdio_pio_host_t;

bool pf_sdio_pio_host_init(pf_sdio_pio_host_t *host,
                              const pf_sdio_pio_config_t *config);
void pf_sdio_pio_host_deinit(pf_sdio_pio_host_t *host);
bool pf_sdio_pio_host_set_clock(pf_sdio_pio_host_t *host, uint32_t clock_hz);
bool pf_sdio_pio_host_command(pf_sdio_pio_host_t *host, uint8_t command,
                                 uint32_t argument, uint16_t response_bits,
                                 uint32_t *response_words, size_t response_word_capacity);
bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity);
bool pf_sdio_pio_host_read_sector(pf_sdio_pio_host_t *host, uint32_t sector,
                                     bool block_addressed, uint8_t block[512]);
/** Read and CRC-check the 64-byte data block returned by SD CMD6. */
bool pf_sdio_pio_host_read_switch_status(pf_sdio_pio_host_t *host, uint32_t argument,
                                         uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE]);
bool pf_sdio_pio_host_read_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
                                      bool block_addressed, uint8_t *blocks,
                                      uint32_t block_count);
bool pf_sdio_pio_host_write_sector(pf_sdio_pio_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512]);
/**
 * Write blocks with CMD25. The transfer stays open after success so a call starting at the next
 * sector continues it without a new CMD25; any other bus access must first call
 * pf_sdio_pio_host_write_stream_close(), which is what commits the data on the card.
 */
bool pf_sdio_pio_host_write_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
                                      bool block_addressed, const uint8_t *blocks,
                                      uint32_t block_count);
/** @return true when a write stream is open and a write at @p sector would continue it. */
bool pf_sdio_pio_host_write_stream_continues(const pf_sdio_pio_host_t *host, uint32_t sector);
/** Stop an open write stream (CMD12) and restore command mode; true if none was open. */
bool pf_sdio_pio_host_write_stream_close(pf_sdio_pio_host_t *host);

#endif