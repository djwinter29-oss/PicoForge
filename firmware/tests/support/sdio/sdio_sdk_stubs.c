#include "sdio_sdk_stubs.h"

#include <stddef.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/regs/pio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "storage_sdio_pio.pio.h"

pf_test_sdio_event_t pf_test_sdio_events[256];
unsigned pf_test_sdio_event_count;
bool pf_test_sdio_trace_enabled;
void (*pf_test_sdio_dma_poll_hook)(void);

static void trace_event(pf_test_sdio_event_t event)
{
    if (pf_test_sdio_trace_enabled) {
        TEST_ASSERT_LESS_THAN_UINT(256u, pf_test_sdio_event_count);
        pf_test_sdio_events[pf_test_sdio_event_count++] = event;
    }
}

const pio_program_t pf_sdio_pio_clock_program = { 0 };
const pio_program_t pf_sdio_pio_command_program = { 0 };
const pio_program_t pf_sdio_pio_data_rx_program = { 0 };
const pio_program_t pf_sdio_pio_data_rx_fast_program = { 0 };
const pio_program_t pf_sdio_pio_data_tx_program = { 0 };
const pio_program_t pf_sdio_pio_data_tx_fast_program = { 0 };
dma_hw_t pf_test_sdio_dma_registers;
unsigned pf_test_sdio_barrier_count;
bool pf_test_sdio_data_irq = true;
bool pf_test_sdio_dma_busy;
uint32_t pf_test_sdio_system_clock_hz = 125000000u;
bool pf_test_sdio_force_tx_stall;
bool pf_test_sdio_corrupt_command_crc;
unsigned pf_test_sdio_data_fifo_words = 1u;
uint32_t pf_test_sdio_data_response = 0x05u;
uint32_t pf_test_sdio_time_step = 1u;
unsigned pf_test_sdio_command_count;
uint8_t pf_test_sdio_last_command;
uint32_t pf_test_sdio_dma_word_count;
dma_channel_config pf_test_sdio_last_dma_config;
uint32_t pf_test_sdio_gpio_level = 1u;
uint32_t pf_test_sdio_clock_gpio = UINT32_MAX;
bool pf_test_sdio_clock_auto_toggle;
unsigned pf_test_sdio_clock_rising_edges;
uint16_t pf_test_sdio_config_divider_integer;
uint8_t pf_test_sdio_config_divider_fraction;
uint32_t pf_test_sdio_clkdiv_restart_mask;
uint32_t pf_test_sdio_sync_mask;
unsigned pf_test_sdio_tx_preload_queries;
unsigned pf_test_sdio_dma_busy_delay;
unsigned pf_test_sdio_data_irq_delay;
unsigned pf_test_sdio_data_irq_queries;
unsigned pf_test_sdio_dma_busy_queries;
unsigned pf_test_sdio_dma_abort_count;
uint8_t pf_test_sdio_rx_wire[40];
size_t pf_test_sdio_rx_wire_size;
bool pf_test_sdio_data_wait_ok = true;
unsigned pf_test_sdio_command_fail;
unsigned pf_test_sdio_command_error;
unsigned pf_test_sdio_corrupt_command;
unsigned pf_test_sdio_pio_restart_calls;
uint32_t pf_test_sdio_pio_trace[32];
unsigned pf_test_sdio_pio_trace_count;
uint8_t pf_test_sdio_batch_rx[PF_TEST_SDIO_MAX_RX_BATCH_BYTES];
size_t pf_test_sdio_batch_rx_size;
static uint32_t pf_test_sdio_command_words[2];
static unsigned pf_test_sdio_command_word_count;
static unsigned pf_test_sdio_command_word_index;
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
static uint claimed_sms[2];
int pio_claim_unused_sm(PIO pio, bool required)
{
    (void)required;
    uint index = pio == pio0 ? 0u : 1u;
    for (uint sm = 0u; sm < 4u; ++sm) {
        if ((claimed_sms[index] & (1u << sm)) == 0u) {
            claimed_sms[index] |= 1u << sm;
            return (int)sm;
        }
    }
    return -1;
}
void pio_sm_unclaim(PIO pio, uint sm)
{
    claimed_sms[pio == pio0 ? 0u : 1u] &= ~(1u << sm);
}
void pio_gpio_init(PIO pio, uint pin) { (void)pio; (void)pin; }
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out)
{ (void)pio; (void)sm; (void)pin; (void)count; (void)out; }
void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config)
{ (void)pio; (void)sm; (void)offset; (void)config; }
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled)
{
    if (sm == 1u) trace_event(enabled ? PF_TEST_RX_ENABLE : PF_TEST_RX_DISABLE);
    if (pf_test_sdio_force_tx_stall && sm == 1u && enabled) {
        pio->fdebug |= 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    }
    (void)pio; (void)sm; (void)enabled;
#if !defined(PF_TEST_SDIO_WIRE_ONLY)
    if (pf_test_sdio_expect_streams_cleared && pf_test_sdio_observed_host != NULL) {
        pf_test_sdio_streams_were_cleared =
            !pf_test_sdio_observed_host->write_stream_open &&
            !pf_test_sdio_observed_host->read_stream_unresolved;
    }
#endif
}
void pio_enable_sm_mask_in_sync(PIO pio, uint mask)
{ (void)pio; pf_test_sdio_sync_mask = mask; }
uint pio_get_dreq(PIO pio, uint sm, bool tx) { (void)pio; (void)sm; (void)tx; return 0u; }
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t value)
{
    (void)pio;
    if (sm == 1u && pf_test_sdio_pio_trace_count < 32u) {
        trace_event(PF_TEST_RX_PUT);
        pf_test_sdio_pio_trace[pf_test_sdio_pio_trace_count++] = 0x10000000u | value;
    }
    if (sm == 0u) {
        if (pf_test_sdio_command_word_count == 0u) {
            pf_test_sdio_last_command = (uint8_t)((value >> 18u) & 0x3fu);
            if (pf_test_sdio_last_command == 18u) trace_event(PF_TEST_CMD18);
            if (pf_test_sdio_last_command == 12u) trace_event(PF_TEST_CMD12);
            ++pf_test_sdio_command_count;
        }
        ++pf_test_sdio_command_word_count;
        if (pf_test_sdio_command_word_count == 3u) {
            uint8_t response[6] = {0};
            response[0] = pf_test_sdio_last_command;
            if (pf_test_sdio_last_command == pf_test_sdio_command_error) {
                response[2] = 0x40u;
            }
#if defined(PF_TEST_SDIO_WIRE_ONLY)
            response[5] = 1u;
#else
            response[5] = (uint8_t)((pf_sd_native_crc7(response, 5u) << 1u) | 1u);
#endif
            if (pf_test_sdio_corrupt_command_crc &&
                ((pf_test_sdio_corrupt_command == 0u) ||
                 (pf_test_sdio_corrupt_command == pf_test_sdio_last_command))) {
                response[5] ^= 2u;
            }
            pf_test_sdio_command_words[0] = ((uint32_t)response[0] << 24u) |
                ((uint32_t)response[1] << 16u) | ((uint32_t)response[2] << 8u) | response[3];
            pf_test_sdio_command_words[1] = ((uint32_t)response[4] << 8u) |
                response[5];
            pf_test_sdio_command_word_index = 0u;
        }
    }
    (void)value;
}
void pio_sm_exec(PIO pio, uint sm, uint instruction)
{
    (void)pio;
    if (sm == 1u && pf_test_sdio_pio_trace_count < 32u) {
        trace_event(PF_TEST_RX_EXEC);
        pf_test_sdio_pio_trace[pf_test_sdio_pio_trace_count++] = 0x20000000u | instruction;
    }
}
void pio_sm_clear_fifos(PIO pio, uint sm)
{ (void)pio; if (sm == 1u) trace_event(PF_TEST_RX_CLEAR); }
void pio_sm_restart(PIO pio, uint sm)
{ (void)pio; if (sm == 1u) { ++pf_test_sdio_pio_restart_calls; trace_event(PF_TEST_RX_RESTART); } }
void pio_clkdiv_restart_sm_mask(PIO pio, uint32_t mask)
{ (void)pio; pf_test_sdio_clkdiv_restart_mask = mask; }
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm)
{
    (void)pio;
    if (sm == 0u) {
        return pf_test_sdio_command_word_count == 3u
            ? 2u - pf_test_sdio_command_word_index : 0u;
    }
    return pf_test_sdio_data_fifo_words;
}
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    ++pf_test_sdio_tx_preload_queries;
    return 1u;
}
uint32_t pio_sm_get(PIO pio, uint sm)
{
    (void)pio;
    if (sm == 0u && pf_test_sdio_command_word_index < 2u) {
        return pf_test_sdio_command_words[pf_test_sdio_command_word_index++];
    }
    return pf_test_sdio_data_response;
}
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t integer, uint8_t fraction)
{ (void)pio; (void)sm; (void)integer; (void)fraction; }
void pio_interrupt_clear(PIO pio, uint index)
{
    (void)pio;
    if (index == 0u) pf_test_sdio_command_word_count = 0u;
    if (index == 1u) trace_event(PF_TEST_RX_IRQ_CLEAR);
}
bool pio_interrupt_get(PIO pio, uint index)
{
    (void)pio;
    if (index == 0u) {
        return pf_test_sdio_last_command != pf_test_sdio_command_fail;
    }
    if (index == 2u) {
        ++pf_test_sdio_data_irq_queries;
        if (pf_test_sdio_data_irq_delay != 0u) {
            --pf_test_sdio_data_irq_delay;
            return false;
        }
        return pf_test_sdio_data_irq;
    }
    return pf_test_sdio_data_wait_ok;
}
uint pio_encode_jmp(uint address) { return address; }
uint pio_encode_pull(bool if_empty, bool block) { return (uint)if_empty | ((uint)block << 1u); }
uint pio_encode_mov(uint destination, uint source) { return destination | (source << 8u); }
uint pio_encode_set(uint destination, uint value)
{ TEST_ASSERT_LESS_OR_EQUAL_UINT(31u, value); return destination | (value << 8u); }
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
{
    (void)config;
    pf_test_sdio_config_divider_integer = integer;
    pf_test_sdio_config_divider_fraction = fraction;
}
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
{
    (void)channel; (void)read_addr; (void)start;
    trace_event(PF_TEST_DMA_CONFIGURE);
    pf_test_sdio_last_dma_config = *config;
    pf_test_sdio_dma_word_count = transfer_count;
    if (config->write_inc) {
        size_t copy_size = transfer_count * sizeof(uint32_t);
        if (pf_test_sdio_batch_rx_size != 0u) {
            if (copy_size > pf_test_sdio_batch_rx_size) copy_size = pf_test_sdio_batch_rx_size;
            memcpy(write_addr, pf_test_sdio_batch_rx, copy_size);
        } else if (pf_test_sdio_rx_wire_size != 0u) {
            if (copy_size > pf_test_sdio_rx_wire_size) copy_size = pf_test_sdio_rx_wire_size;
            memcpy(write_addr, pf_test_sdio_rx_wire, copy_size);
        }
        pf_test_sdio_dma_registers.write_addr =
            (uintptr_t)write_addr + transfer_count * sizeof(uint32_t);
    }
}
void dma_channel_start(unsigned channel) { (void)channel; trace_event(PF_TEST_DMA_START); }
bool dma_channel_is_busy(unsigned channel)
{
    (void)channel;
    if (pf_test_sdio_dma_poll_hook != NULL) pf_test_sdio_dma_poll_hook();
    ++pf_test_sdio_dma_busy_queries;
    if (pf_test_sdio_dma_busy_delay != 0u) {
        --pf_test_sdio_dma_busy_delay;
        return true;
    }
    return pf_test_sdio_dma_busy;
}
void dma_channel_abort(unsigned channel)
{
    (void)channel;
    ++pf_test_sdio_dma_abort_count;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_dma_busy_delay = 0u;
    trace_event(PF_TEST_DMA_ABORT);
}
void dma_channel_wait_for_finish_blocking(unsigned channel) { (void)channel; }
dma_hw_t *dma_channel_hw_addr(unsigned channel) { (void)channel; return &pf_test_sdio_dma_registers; }
int dma_claim_unused_channel(bool required) { (void)required; return 0; }
void dma_channel_unclaim(unsigned channel) { (void)channel; }

uint32_t clock_get_hz(unsigned clock) { (void)clock; return pf_test_sdio_system_clock_hz; }
uint32_t gpio_get(unsigned pin)
{
    if (pf_test_sdio_clock_auto_toggle && (pin == pf_test_sdio_clock_gpio)) {
        pf_test_sdio_gpio_level ^= 1u;
        if (pf_test_sdio_gpio_level != 0u) {
            ++pf_test_sdio_clock_rising_edges;
        }
        return pf_test_sdio_gpio_level;
    }
    return pf_test_sdio_gpio_level;
}
void gpio_pull_up(unsigned pin) { (void)pin; }
void gpio_init(unsigned pin) { (void)pin; }
void gpio_put(unsigned pin, bool value) { (void)pin; (void)value; }
void gpio_set_dir(unsigned pin, bool out) { (void)pin; (void)out; }
void gpio_pull_down(unsigned pin) { (void)pin; }
void gpio_disable_pulls(unsigned pin) { (void)pin; }
void __dmb(void) { ++pf_test_sdio_barrier_count; trace_event(PF_TEST_BARRIER); }
uint32_t time_us_32(void) { static uint32_t now; now += pf_test_sdio_time_step; return now; }
void sleep_us(uint64_t duration) { (void)duration; }
void sleep_ms(uint32_t duration) { (void)duration; }
void tight_loop_contents(void) {}
