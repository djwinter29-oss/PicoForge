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
    pf_sdio_pio_lane_map_t lane_map;
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
    bool command_program_loaded;
    bool data_rx_program_loaded;
    bool clock_program_loaded;
    bool data_tx_program_loaded;
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
bool pf_sdio_pio_host_write_sector(pf_sdio_pio_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512]);

#endif