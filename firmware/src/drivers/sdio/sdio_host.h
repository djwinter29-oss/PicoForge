#ifndef PICOFORGE_SDIO_HOST_H
#define PICOFORGE_SDIO_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdio_pio.h"

typedef struct {
    PIO pio;
    uint command_gpio;
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
    bool command_program_loaded;
    bool data_rx_program_loaded;
    bool clock_program_loaded;
    bool data_tx_program_loaded;
} picoforge_sdio_host_t;

bool picoforge_sdio_host_init(picoforge_sdio_host_t *host,
                              const picoforge_sdio_config_t *config);
void picoforge_sdio_host_deinit(picoforge_sdio_host_t *host);
bool picoforge_sdio_host_set_clock(picoforge_sdio_host_t *host, uint32_t clock_hz);
bool picoforge_sdio_host_command(picoforge_sdio_host_t *host, uint8_t command,
                                 uint32_t argument, uint16_t response_bits,
                                 uint32_t *response_words, size_t response_word_capacity);
bool picoforge_sdio_host_command_bytes(picoforge_sdio_host_t *host, uint8_t command,
                                       uint32_t argument, uint16_t response_bits,
                                       uint8_t *response, size_t response_capacity);
bool picoforge_sdio_host_read_sector(picoforge_sdio_host_t *host, uint32_t sector,
                                     bool block_addressed, uint8_t block[512]);
bool picoforge_sdio_host_write_sector(picoforge_sdio_host_t *host, uint32_t sector,
                                      bool block_addressed, const uint8_t block[512]);

#endif