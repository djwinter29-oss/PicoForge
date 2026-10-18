#include "unity.h"
void pf_sd_lock(void) {}
void pf_sd_unlock(void) {}
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"
#include "sdio_pio.h"

static pf_sdio_pio_t test_wire;
#include "sdio_storage_limits.h"

#include <stdint.h>
#include <string.h>

#include "drivers/storage/sdio_pio/sdio_pio.c"
#include "drivers/storage/bus.c"
#include "drivers/storage/sd_wire.c"
#include "drivers/sd_card/sd_native_host.c"
#include "drivers/sd_card/sd_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sd_native_host_t *pf_test_sdio_observed_host;
bool pf_test_sdio_expect_streams_cleared;
bool pf_test_sdio_streams_were_cleared;
bool pf_test_sdio_can_add_program;
unsigned pf_test_sdio_removed_programs;

void setUp(void)
{
    memset(read_wire_buffer, 0, sizeof(read_wire_buffer));
    memset(&pf_test_sdio_dma_registers, 0, sizeof(pf_test_sdio_dma_registers));
    pf_test_sdio_batch_rx_size = 0u;
    pf_test_sdio_observed_host = NULL;
    pf_test_sdio_expect_streams_cleared = false;
    pf_test_sdio_streams_were_cleared = false;
    pf_test_sdio_can_add_program = true;
    pf_test_sdio_system_clock_hz = 125000000u;
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_barrier_count = 0u;
    pf_test_sdio_pio_trace_count = 0u;
    pf_test_sdio_command_count = 0u;
    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_command_error = 0u;
    pf_test_sdio_corrupt_command_crc = false;
    pf_test_sdio_data_irq = true;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_dma_abort_count = 0u;
    pf_test_sdio_data_wait_ok = true;
    pf_test_sdio_trace_enabled = true;
    pf_test_sdio_event_count = 0u;
    pf_test_sdio_dma_poll_hook = NULL;
    pf_test_sdio_dma_busy_delay = 0u;
    pf_test_sdio_clock_gpio = UINT32_MAX;
    pf_test_sdio_clock_auto_toggle = false;
    pf_test_sdio_clock_rising_edges = 0u;
    pf_test_sdio_gpio_level = 1u;
    pf_test_sdio_config_divider_integer = 0u;
    pf_test_sdio_config_divider_fraction = 0u;
    pf_test_sdio_clkdiv_restart_mask = 0u;
    pf_test_sdio_sync_mask = 0u;
}

void tearDown(void)
{
    pf_test_sdio_trace_enabled = false;
    pf_test_sdio_dma_poll_hook = NULL;
}

void test_host_clock_uses_integer_rounding_through_25mhz_and_fractional_fast_mode(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){.pio = &pio, .system_clock_hz = 250000000u};
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));

    TEST_ASSERT_TRUE(pf_sd_native_host_set_clock(&host, 25000000u));
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(5u * 256u, test_wire.clock_div_ticks);
    TEST_ASSERT_TRUE(pf_sd_native_host_set_clock(&host, 50000000u));
    TEST_ASSERT_EQUAL_UINT32(50000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(640u, test_wire.clock_div_ticks);
    TEST_ASSERT_TRUE(test_wire.data_rx_fast);

    test_wire.system_clock_hz = 125000000u;
    TEST_ASSERT_TRUE(pf_sd_native_host_set_clock(&host, 25000000u));
    TEST_ASSERT_EQUAL_UINT32(20833333u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(3u * 256u, test_wire.clock_div_ticks);
}

void test_prepare_initialization_resets_host_and_emits_exactly_eighty_low_speed_clocks(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .config.pins.command_gpio = 1u,
        .config.pins.data_gpio[0] = 2u,
        .config.pins.clock_gpio = 5u,
        .command_sm = 0u,
        .data_sm = 1u,
        .command_program_offset = 1u,
        .data_rx_program_offset = 2u,
        .clock_program_offset = 3u,
        .data_tx_program_offset = 4u,
        .dma_channel = 3u,
        .system_clock_hz = 150000000u,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
    };
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire), .bus_width = 4u };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));

    pf_test_sdio_clock_gpio = test_wire.config.pins.clock_gpio;
    pf_test_sdio_gpio_level = 0u;
    pf_test_sdio_clock_auto_toggle = true;
    TEST_ASSERT_TRUE(pf_sd_native_host_prepare_initialization(&host));
    TEST_ASSERT_EQUAL_UINT(80u, pf_test_sdio_clock_rising_edges);
    TEST_ASSERT_TRUE(test_wire.current_clock_hz > 0u && test_wire.current_clock_hz <= 400000u);
    TEST_ASSERT_EQUAL(PF_SDIO_PIO_CLOCK_UNOWNED, test_wire.clock_owner);
    TEST_ASSERT_EQUAL_UINT(1u, host.bus_width);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_NONE, host.last_error);
}

void test_abort_preserves_diagnostics_and_unresolved_streams_without_card_commands(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .command_program_offset = 1u,
        .data_rx_program_offset = 2u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
    };
    pf_sd_native_host_t host = {
        .transport = pf_sdio_pio_bus(&test_wire),
        .write_stream_open = true,
        .read_stream_unresolved = true,
        .last_error = PF_SD_DETAIL_DMA_TIMEOUT,
        .errors = { .data_timeouts = 3u, .dma_timeouts = 2u },
    };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));

    pf_sd_native_host_abort(&host);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DMA_TIMEOUT, host.last_error);
    TEST_ASSERT_EQUAL_UINT32(3u, host.errors.data_timeouts);
    TEST_ASSERT_EQUAL_UINT32(2u, host.errors.dma_timeouts);
    TEST_ASSERT_TRUE(host.write_stream_open);
    TEST_ASSERT_TRUE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
    TEST_ASSERT_EQUAL_UINT(1u, pf_test_sdio_dma_abort_count);
    TEST_ASSERT_EQUAL(PF_SDIO_PIO_CLOCK_UNOWNED, test_wire.clock_owner);
}

static void encode_test_blocks(uint8_t blocks[4][PF_SD_BLOCK_SIZE])
{
    for (unsigned block = 0u; block < 4u; ++block) {
        for (unsigned byte = 0u; byte < PF_SD_BLOCK_SIZE; ++byte) {
            blocks[block][byte] = (uint8_t)(byte * 31u + block);
        }
        TEST_ASSERT_TRUE(pf_sd_native_encode_data_block(
            blocks[block], (uint8_t *)&read_wire_buffer[block * SDIO_DATA_WORD_COUNT]));
    }
}

static bool decode_at(pf_sd_native_host_t *host, uint32_t transferred_words,
                      uint8_t *blocks, uint32_t capacity_words, uint32_t *decoded_count)
{
    uint32_t buffer_start = (uint32_t)(uintptr_t)read_wire_buffer;
    pf_test_sdio_dma_registers.write_addr = buffer_start + transferred_words * sizeof(uint32_t);
    return decode_completed_blocks(host, blocks, capacity_words, decoded_count);
}

void test_read_batch_decode_tracks_complete_blocks_and_crc_boundaries(void)
{
    test_wire = (pf_sdio_pio_t){.dma_channel = 3u, .dma_claimed = true};
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t wire_blocks[4][PF_SD_BLOCK_SIZE];
    uint8_t decoded[4][PF_SD_BLOCK_SIZE] = {{0}};
    uint32_t decoded_count = 0u;

    encode_test_blocks(wire_blocks);

    TEST_ASSERT_TRUE(decode_at(&host, 0u, &decoded[0][0],
                               4u * SDIO_DATA_WORD_COUNT, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(0u, decoded_count);
    TEST_ASSERT_TRUE(decode_at(&host, SDIO_DATA_WORD_COUNT - 1u, &decoded[0][0],
                               4u * SDIO_DATA_WORD_COUNT, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(0u, decoded_count);
    TEST_ASSERT_TRUE(decode_at(&host, 3u * SDIO_DATA_WORD_COUNT - 1u, &decoded[0][0],
                               4u * SDIO_DATA_WORD_COUNT, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(2u, decoded_count);
    TEST_ASSERT_TRUE(decode_at(&host, SDIO_DATA_WORD_COUNT, &decoded[0][0],
                               4u * SDIO_DATA_WORD_COUNT, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(2u, decoded_count);
    TEST_ASSERT_TRUE(decode_at(&host, 4u * SDIO_DATA_WORD_COUNT, &decoded[0][0],
                               4u * SDIO_DATA_WORD_COUNT, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(4u, decoded_count);
    TEST_ASSERT_EQUAL_MEMORY(&wire_blocks[0][0], &decoded[0][0], sizeof(decoded));
    TEST_ASSERT_TRUE(pf_test_sdio_barrier_count >= 5u);
}

void test_read_batch_decode_rejects_invalid_dma_addresses_and_crc(void)
{
    test_wire = (pf_sdio_pio_t){.dma_channel = 3u, .dma_claimed = true};
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t wire_blocks[4][PF_SD_BLOCK_SIZE];
    uint8_t decoded[4][PF_SD_BLOCK_SIZE] = {{0}};
    uint32_t decoded_count = 0u;
    uint32_t buffer_start = (uint32_t)(uintptr_t)read_wire_buffer;
    const uint32_t capacity_words = 4u * SDIO_DATA_WORD_COUNT;

    encode_test_blocks(wire_blocks);

    pf_test_sdio_dma_registers.write_addr = buffer_start - sizeof(uint32_t);
    TEST_ASSERT_FALSE(decode_completed_blocks(&host, decoded[0], capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(0u, decoded_count);

    TEST_ASSERT_FALSE(decode_at(&host, capacity_words + 1u, decoded[0],
                                capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(0u, decoded_count);

    pf_test_sdio_dma_registers.write_addr = buffer_start + 1u;
    TEST_ASSERT_FALSE(decode_completed_blocks(&host, decoded[0], capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(0u, decoded_count);

    read_wire_buffer[SDIO_DATA_WORD_COUNT + 1u] ^= 1u;
    TEST_ASSERT_FALSE(decode_at(&host, capacity_words, decoded[0],
                                capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(1u, decoded_count);

    decoded_count = 0u;
    encode_test_blocks(wire_blocks);
    TEST_ASSERT_TRUE(decode_at(&host, capacity_words, NULL,
                               capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(4u, decoded_count);

    decoded_count = 0u;
    read_wire_buffer[SDIO_DATA_WORD_COUNT + 1u] ^= 1u;
    TEST_ASSERT_FALSE(decode_at(&host, capacity_words, NULL,
                                capacity_words, &decoded_count));
    TEST_ASSERT_EQUAL_UINT32(1u, decoded_count);
}

void test_single_read_crc_is_counted_without_profiling_and_requires_four_bit_mode(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .command_program_offset = 1u,
        .data_rx_program_offset = 2u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u,
    };
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire), .bus_width = 4u };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t block[PF_SD_BLOCK_SIZE] = {0};
    uint8_t payload[PF_SD_BLOCK_SIZE];

    pf_sd_set_profiling_enabled(false);
    TEST_ASSERT_TRUE(pf_sd_native_encode_data_block(
        block, pf_test_sdio_batch_rx));
    pf_test_sdio_batch_rx_size = PF_SD_NATIVE_IMAGE_SIZE;
    pf_test_sdio_batch_rx[PF_SD_NATIVE_IMAGE_SIZE - 1u] ^= 1u;
    TEST_ASSERT_FALSE(pf_sd_native_host_read_sector(&host, 7u, payload));
    TEST_ASSERT_EQUAL_UINT32(1u, host.errors.data_crc_errors);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, host.last_error);

    host.bus_width = 1u;
    pf_test_sdio_command_count = 0u;
    TEST_ASSERT_FALSE(pf_sd_native_host_read_sector(&host, 7u, payload));
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_FOUR_BIT, host.last_error);
    TEST_ASSERT_EQUAL_UINT(0u, pf_test_sdio_command_count);
}

void test_read_batch_loads_pio_count_before_dma_and_framing(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
    };
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint32_t destination[SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS];

    TEST_ASSERT_TRUE(pf_sdio_pio_rx_start(&test_wire, destination,
                  SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS,
                  SDIO_DATA_NIBBLE_COUNT, SDIO_READ_BATCH_BLOCKS));

    TEST_ASSERT_EQUAL_UINT(5u, pf_test_sdio_pio_trace_count);
    TEST_ASSERT_EQUAL_UINT32(0x20000004u, pf_test_sdio_pio_trace[0]);
    TEST_ASSERT_EQUAL_UINT32(0x1000003fu, pf_test_sdio_pio_trace[1]);
    TEST_ASSERT_EQUAL_UINT32(0x20000002u, pf_test_sdio_pio_trace[2]);
    TEST_ASSERT_EQUAL_UINT32(0x20000302u, pf_test_sdio_pio_trace[3]);
    TEST_ASSERT_EQUAL_UINT32(0x1000040fu, pf_test_sdio_pio_trace[4]);
    TEST_ASSERT_EQUAL_UINT32(SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS,
                             pf_test_sdio_dma_word_count);
    TEST_ASSERT_EQUAL_UINT(DMA_SIZE_32, pf_test_sdio_last_dma_config.size);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.swap);
    TEST_ASSERT_TRUE(pf_test_sdio_last_dma_config.write_inc);
    TEST_ASSERT_FALSE(pf_test_sdio_last_dma_config.read_inc);
    const pf_test_sdio_event_t expected[] = {
        PF_TEST_RX_DISABLE, PF_TEST_RX_CLEAR, PF_TEST_RX_IRQ_CLEAR, PF_TEST_RX_RESTART,
        PF_TEST_RX_EXEC, PF_TEST_RX_PUT, PF_TEST_RX_EXEC, PF_TEST_RX_EXEC,
        PF_TEST_RX_PUT, PF_TEST_DMA_CONFIGURE, PF_TEST_DMA_START, PF_TEST_RX_ENABLE
    };
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) / sizeof(expected[0]), pf_test_sdio_event_count);
    TEST_ASSERT_EQUAL_MEMORY(expected, pf_test_sdio_events, sizeof(expected));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(PF_FILE_BENCHMARK_BUFFER_SIZE,
                                  SDIO_READ_BATCH_BLOCKS * PF_SD_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_UINT(0u, PF_FILE_BENCHMARK_BUFFER_SIZE % PF_SD_BLOCK_SIZE);
}

void test_rx_cleanup_aborts_dma_before_clearing_and_restarting_pio(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){.pio = &pio, .data_sm = 1u, .dma_channel = 3u, .dma_claimed = true};
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    pf_test_sdio_dma_busy = true;
    pf_sdio_pio_rx_cleanup(&test_wire);
    const pf_test_sdio_event_t expected[] = {
        PF_TEST_RX_DISABLE, PF_TEST_DMA_ABORT, PF_TEST_RX_CLEAR,
        PF_TEST_RX_IRQ_CLEAR, PF_TEST_RX_RESTART, PF_TEST_RX_EXEC
    };
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) / sizeof(expected[0]), pf_test_sdio_event_count);
    TEST_ASSERT_EQUAL_MEMORY(expected, pf_test_sdio_events, sizeof(expected));
    pf_test_sdio_dma_busy = false;
}

void test_stock_system_clock_produces_exact_qualified_bus_clock(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){.pio = &pio, .system_clock_hz = 150000000u};
    pf_sd_native_host_t host = { .transport = pf_sdio_pio_bus(&test_wire) };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    TEST_ASSERT_TRUE(pf_sd_native_host_set_clock(&host, 25000000u));
    TEST_ASSERT_EQUAL_UINT32(3u * 256u, test_wire.clock_div_ticks);
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
}

static void make_receive_batch(const uint8_t blocks[][PF_SD_BLOCK_SIZE], unsigned count)
{
    for (unsigned block = 0u; block < count; ++block) {
        TEST_ASSERT_TRUE(pf_sd_native_encode_data_block(
            blocks[block],
            &pf_test_sdio_batch_rx[block * PF_SD_NATIVE_IMAGE_SIZE]));
    }
    pf_test_sdio_batch_rx_size = count * PF_SD_NATIVE_IMAGE_SIZE;
}

static unsigned overlap_polls;

static void observe_overlapped_decode(void)
{
    if (++overlap_polls == 2u) {
        TEST_ASSERT_GREATER_THAN_UINT(0u, pf_test_sdio_barrier_count);
        TEST_ASSERT_TRUE(pf_test_sdio_dma_busy_delay > 0u);
    }
}

void test_production_cmd18_reader_decodes_all_blocks_then_stops_stream(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u,
    };
    pf_sd_native_host_t host = {
        .transport = pf_sdio_pio_bus(&test_wire),
        .data_rx_multiblock_supported = true,
        .bus_width = 4u,
    };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t expected[2][PF_SD_BLOCK_SIZE];
    uint8_t received[2][PF_SD_BLOCK_SIZE] = {{0}};

    for (unsigned i = 0u; i < sizeof(expected); ++i) {
        ((uint8_t *)expected)[i] = (uint8_t)(i * 19u + 3u);
    }
    make_receive_batch(expected, 2u);
    overlap_polls = 0u;
    pf_test_sdio_dma_busy_delay = 2u;
    pf_test_sdio_dma_poll_hook = observe_overlapped_decode;

    TEST_ASSERT_TRUE(pf_sd_native_host_read_blocks(&host, 17u,
                                                   &received[0][0], 2u));
    TEST_ASSERT_EQUAL_MEMORY(expected, received, sizeof(expected));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2u, overlap_polls);
    unsigned command_event = 0u, decode_event = 0u, cleanup_event = 0u, stop_event = 0u;
    for (unsigned i = 0u; i < pf_test_sdio_event_count; ++i) {
        if (pf_test_sdio_events[i] == PF_TEST_CMD18) command_event = i;
        if (pf_test_sdio_events[i] == PF_TEST_BARRIER && decode_event == 0u) decode_event = i;
        if (pf_test_sdio_events[i] == PF_TEST_RX_DISABLE) cleanup_event = i;
        if (pf_test_sdio_events[i] == PF_TEST_CMD12) stop_event = i;
    }
    TEST_ASSERT_TRUE(command_event > 0u && command_event < decode_event);
    TEST_ASSERT_TRUE(decode_event < cleanup_event && cleanup_event < stop_event);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_command_count);
    TEST_ASSERT_EQUAL_UINT8(12u, pf_test_sdio_last_command);
}

void test_production_cmd18_reader_stops_after_crc_failure_and_preserves_unresolved_state(void)
{
    pf_test_sdio_pio_t pio = {0};
    test_wire = (pf_sdio_pio_t){
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u,
        .command_program_loaded = true,
        .data_rx_program_loaded = true,
        .dma_claimed = true,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u,
    };
    pf_sd_native_host_t host = {
        .transport = pf_sdio_pio_bus(&test_wire),
        .data_rx_multiblock_supported = true,
        .bus_width = 4u,
    };
    TEST_ASSERT_TRUE(pf_storage_sd_wire_hooks(&host.transport, transport_error, &host, pf_sd_idle));
    uint8_t expected[2][PF_SD_BLOCK_SIZE] = {{0}};
    uint8_t received[2][PF_SD_BLOCK_SIZE] = {{0}};

    make_receive_batch(expected, 2u);
    pf_test_sdio_batch_rx[PF_SD_NATIVE_IMAGE_SIZE + PF_SD_BLOCK_SIZE] ^= 1u;

    TEST_ASSERT_FALSE(pf_sd_native_host_read_blocks(&host, 17u,
                                                    &received[0][0], 2u));
    TEST_ASSERT_EQUAL_UINT32(1u, host.errors.data_crc_errors);
    TEST_ASSERT_EQUAL(PF_SD_DETAIL_DATA_CRC, host.last_error);
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT8(12u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_command_count);

    setUp();
    host.read_stream_unresolved = false;
    make_receive_batch(expected, 2u);
    pf_test_sdio_command_error = 12u;
    TEST_ASSERT_FALSE(pf_sd_native_host_read_blocks(&host, 17u,
                                                    &received[0][0], 2u));
    TEST_ASSERT_TRUE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT32(25000000u, test_wire.current_clock_hz);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_command_count);
}
