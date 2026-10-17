#ifndef PF_TEST_SDIO_HARDWARE_DMA_H
#define PF_TEST_SDIO_HARDWARE_DMA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    unsigned size;
    unsigned dreq;
    bool swap;
    bool read_inc;
    bool write_inc;
} dma_channel_config;

typedef struct {
    uintptr_t write_addr;
} dma_hw_t;

enum { DMA_SIZE_32 = 2u };

dma_channel_config dma_channel_get_default_config(unsigned channel);
void channel_config_set_transfer_data_size(dma_channel_config *config, unsigned size);
void channel_config_set_dreq(dma_channel_config *config, unsigned dreq);
void channel_config_set_bswap(dma_channel_config *config, bool enabled);
void channel_config_set_read_increment(dma_channel_config *config, bool enabled);
void channel_config_set_write_increment(dma_channel_config *config, bool enabled);
void dma_channel_configure(unsigned channel, const dma_channel_config *config,
                           void *write_addr, const void *read_addr,
                           unsigned transfer_count, bool start);
void dma_channel_start(unsigned channel);
bool dma_channel_is_busy(unsigned channel);
void dma_channel_abort(unsigned channel);
void dma_channel_wait_for_finish_blocking(unsigned channel);
dma_hw_t *dma_channel_hw_addr(unsigned channel);
int dma_claim_unused_channel(bool required);
void dma_channel_unclaim(unsigned channel);

#endif
