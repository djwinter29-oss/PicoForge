#include "sdio_sdk_stubs.h"

#include <stddef.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "sdio_pio_host.pio.h"

const pio_program_t pf_sdio_pio_clock_program = { 0 };
const pio_program_t pf_sdio_pio_command_program = { 0 };
const pio_program_t pf_sdio_pio_data_rx_program = { 0 };
const pio_program_t pf_sdio_pio_data_rx_fast_program = { 0 };
const pio_program_t pf_sdio_pio_data_tx_program = { 0 };
const pio_program_t pf_sdio_pio_data_tx_fast_program = { 0 };
static pf_test_sdio_pio_t pf_test_sdio_pio0;
static pf_test_sdio_pio_t pf_test_sdio_pio1;
PIO pio0 = &pf_test_sdio_pio0;
PIO pio1 = &pf_test_sdio_pio1;

pio_sm_config pf_sdio_pio_clock_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }
pio_sm_config pf_sdio_pio_command_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }
pio_sm_config pf_sdio_pio_data_rx_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }
pio_sm_config pf_sdio_pio_data_rx_fast_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }
pio_sm_config pf_sdio_pio_data_tx_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }
pio_sm_config pf_sdio_pio_data_tx_fast_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){0}; }

bool pio_can_add_program(PIO pio, const pio_program_t *program)
{ (void)pio; (void)program; return pf_test_sdio_can_add_program; }
uint pio_add_program(PIO pio, const pio_program_t *program)
{ (void)pio; (void)program; return 1u; }
void pio_remove_program(PIO pio, const pio_program_t *program, uint offset)
{ (void)pio; (void)program; (void)offset; ++pf_test_sdio_removed_programs; }
uint pio_claim_unused_sm(PIO pio, bool required)
{ (void)pio; (void)required; return 0u; }
void pio_sm_unclaim(PIO pio, uint sm) { (void)pio; (void)sm; }
void pio_gpio_init(PIO pio, uint pin) { (void)pio; (void)pin; }
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out)
{ (void)pio; (void)sm; (void)pin; (void)count; (void)out; }
void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config)
{ (void)pio; (void)sm; (void)offset; (void)config; }
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled)
{
    (void)pio; (void)sm; (void)enabled;
    if (pf_test_sdio_expect_streams_cleared && pf_test_sdio_observed_host != NULL) {
        pf_test_sdio_streams_were_cleared =
            !pf_test_sdio_observed_host->write_stream_open &&
            !pf_test_sdio_observed_host->read_stream_unresolved;
    }
}
void pio_enable_sm_mask_in_sync(PIO pio, uint mask) { (void)pio; (void)mask; }
uint pio_get_dreq(PIO pio, uint sm, bool tx) { (void)pio; (void)sm; (void)tx; return 0u; }
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t value)
{ (void)pio; (void)sm; (void)value; }
void pio_sm_exec(PIO pio, uint sm, uint instruction) { (void)pio; (void)sm; (void)instruction; }
void pio_sm_clear_fifos(PIO pio, uint sm) { (void)pio; (void)sm; }
void pio_sm_restart(PIO pio, uint sm) { (void)pio; (void)sm; }
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm) { (void)pio; (void)sm; return 0u; }
uint32_t pio_sm_get(PIO pio, uint sm) { (void)pio; (void)sm; return 0u; }
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t integer, uint8_t fraction)
{ (void)pio; (void)sm; (void)integer; (void)fraction; }
void pio_interrupt_clear(PIO pio, uint index) { (void)pio; (void)index; }
bool pio_interrupt_get(PIO pio, uint index) { (void)pio; (void)index; return true; }
uint pio_encode_jmp(uint address) { return address; }
uint pio_encode_pull(bool if_empty, bool block) { return (uint)if_empty | ((uint)block << 1u); }
uint pio_encode_mov(uint destination, uint source) { return destination | (source << 8u); }
uint pio_encode_set(uint destination, uint value) { return destination | (value << 8u); }
pio_sm_config pio_get_default_sm_config(void) { return (pio_sm_config){0}; }
uint pio_encode_in(uint source, uint count) { return source | (count << 8u); }
uint pio_encode_out(uint destination, uint count) { return destination | (count << 8u); }
uint pio_encode_wait_gpio(bool polarity, uint pin) { return (uint)polarity | (pin << 8u); }
uint pio_encode_wait_pin(bool polarity, uint pin) { return (uint)polarity | (pin << 8u); }
uint pio_encode_wait_irq(bool polarity, bool relative, uint irq)
{ return (uint)polarity | ((uint)relative << 1u) | (irq << 8u); }
uint pio_encode_irq_set(bool relative, uint irq) { return (uint)relative | (irq << 8u); }
uint pio_encode_irq_wait(bool relative, uint irq) { return (uint)relative | (irq << 8u); }
uint pio_encode_nop(void) { return 0u; }
uint pio_encode_sideset(uint sideset) { return sideset; }
void sm_config_set_in_pins(pio_sm_config *config, uint pin) { (void)config; (void)pin; }
void sm_config_set_out_pins(pio_sm_config *config, uint pin, uint count)
{ (void)config; (void)pin; (void)count; }
void sm_config_set_set_pins(pio_sm_config *config, uint pin, uint count)
{ (void)config; (void)pin; (void)count; }
void sm_config_set_sideset_pins(pio_sm_config *config, uint pin) { (void)config; (void)pin; }
void sm_config_set_in_shift(pio_sm_config *config, bool right, bool autopush, uint threshold)
{ (void)config; (void)right; (void)autopush; (void)threshold; }
void sm_config_set_out_shift(pio_sm_config *config, bool right, bool autopull, uint threshold)
{ (void)config; (void)right; (void)autopull; (void)threshold; }
void sm_config_set_fifo_join(pio_sm_config *config, uint join) { (void)config; (void)join; }
void sm_config_set_clkdiv_int_frac(pio_sm_config *config, uint16_t integer, uint8_t fraction)
{ (void)config; (void)integer; (void)fraction; }
void sm_config_set_sideset(pio_sm_config *config, uint count, bool optional, bool pindirs)
{ (void)config; (void)count; (void)optional; (void)pindirs; }
void sm_config_set_jmp_pin(pio_sm_config *config, uint pin) { (void)config; (void)pin; }
void sm_config_set_mov_status(pio_sm_config *config, uint selector, uint threshold)
{ (void)config; (void)selector; (void)threshold; }

dma_channel_config dma_channel_get_default_config(unsigned channel)
{ (void)channel; return (dma_channel_config){0}; }
void channel_config_set_transfer_data_size(dma_channel_config *config, unsigned size)
{ config->size = size; }
void channel_config_set_dreq(dma_channel_config *config, unsigned dreq) { config->dreq = dreq; }
void channel_config_set_bswap(dma_channel_config *config, bool enabled) { config->swap = enabled; }
void channel_config_set_read_increment(dma_channel_config *config, bool enabled)
{ config->read_inc = enabled; }
void channel_config_set_write_increment(dma_channel_config *config, bool enabled)
{ config->write_inc = enabled; }
void dma_channel_configure(unsigned channel, const dma_channel_config *config,
                           void *write_addr, const void *read_addr,
                           unsigned transfer_count, bool start)
{ (void)channel; (void)config; (void)write_addr; (void)read_addr; (void)transfer_count; (void)start; }
void dma_channel_start(unsigned channel) { (void)channel; }
bool dma_channel_is_busy(unsigned channel) { (void)channel; return false; }
void dma_channel_abort(unsigned channel) { (void)channel; }
void dma_channel_wait_for_finish_blocking(unsigned channel) { (void)channel; }
dma_hw_t *dma_channel_hw_addr(unsigned channel) { (void)channel; return NULL; }
int dma_claim_unused_channel(bool required) { (void)required; return 0; }
void dma_channel_unclaim(unsigned channel) { (void)channel; }

uint32_t clock_get_hz(unsigned clock) { (void)clock; return 125000000u; }
uint32_t gpio_get(unsigned pin) { (void)pin; return 1u; }
void gpio_pull_up(unsigned pin) { (void)pin; }
void __dmb(void) {}
uint32_t time_us_32(void) { static uint32_t now; return ++now; }
void sleep_us(uint64_t duration) { (void)duration; }
void tight_loop_contents(void) {}
