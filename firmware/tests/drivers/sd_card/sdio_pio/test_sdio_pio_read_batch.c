#include "unity.h"
TEST_INCLUDE_PATH("firmware/tests/support/sdio")

#include "sdio_sdk_stubs.h"

#include <stdint.h>
#include <string.h>

#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_host.c"
#include "../../../../src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c"
#include "../../../../src/drivers/sd_card/sd_card_protocol.c"
#include "sdio_sdk_stubs.c"

pf_sdio_pio_host_t *pf_test_sdio_observed_host;
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
    pf_test_sdio_removed_programs = 0u;
    pf_test_sdio_barrier_count = 0u;
    pf_test_sdio_pio_trace_count = 0u;
    pf_test_sdio_command_count = 0u;
    pf_test_sdio_command_fail = 0u;
    pf_test_sdio_command_error = 0u;
    pf_test_sdio_corrupt_command_crc = false;
    pf_test_sdio_data_irq = true;
    pf_test_sdio_dma_busy = false;
    pf_test_sdio_data_wait_ok = true;
}

void tearDown(void) {}

static void encode_test_blocks(uint8_t blocks[4][PF_SD_CARD_BLOCK_SIZE])
{
    for (unsigned block = 0u; block < 4u; ++block) {
        for (unsigned byte = 0u; byte < PF_SD_CARD_BLOCK_SIZE; ++byte) {
            blocks[block][byte] = (uint8_t)(byte * 31u + block);
        }
        TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(
            blocks[block], (uint8_t *)&read_wire_buffer[block * SDIO_DATA_WORD_COUNT]));
    }
}

static bool decode_at(pf_sdio_pio_host_t *host, uint32_t transferred_words,
                      uint8_t *blocks, uint32_t capacity_words, uint32_t *decoded_count)
{
    uint32_t buffer_start = (uint32_t)(uintptr_t)read_wire_buffer;
    pf_test_sdio_dma_registers.write_addr = buffer_start + transferred_words * sizeof(uint32_t);
    return decode_completed_blocks(host, blocks, capacity_words, decoded_count);
}

void test_read_batch_decode_tracks_complete_blocks_and_crc_boundaries(void)
{
    pf_sdio_pio_host_t host = { .dma_channel = 3u };
    uint8_t wire_blocks[4][PF_SD_CARD_BLOCK_SIZE];
    uint8_t decoded[4][PF_SD_CARD_BLOCK_SIZE] = {{0}};
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
    pf_sdio_pio_host_t host = { .dma_channel = 3u };
    uint8_t wire_blocks[4][PF_SD_CARD_BLOCK_SIZE];
    uint8_t decoded[4][PF_SD_CARD_BLOCK_SIZE] = {{0}};
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

void test_read_batch_loads_pio_count_before_dma_and_framing(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = {
        .pio = &pio,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u
    };
    uint32_t destination[SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS];

    start_data_rx(&host, destination,
                  SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS,
                  SDIO_DATA_NIBBLE_COUNT, SDIO_READ_BATCH_BLOCKS);

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
}

static void make_receive_batch(const uint8_t blocks[][PF_SD_CARD_BLOCK_SIZE], unsigned count)
{
    for (unsigned block = 0u; block < count; ++block) {
        TEST_ASSERT_TRUE(pf_sdio_pio_encode_data_block(
            blocks[block],
            &pf_test_sdio_batch_rx[block * PF_SDIO_PIO_IMAGE_SIZE]));
    }
    pf_test_sdio_batch_rx_size = count * PF_SDIO_PIO_IMAGE_SIZE;
}

void test_production_cmd18_reader_decodes_all_blocks_then_stops_stream(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = {
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u,
        .data_rx_program_loaded = true,
        .data_rx_multiblock_supported = true,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u
    };
    uint8_t expected[2][PF_SD_CARD_BLOCK_SIZE];
    uint8_t received[2][PF_SD_CARD_BLOCK_SIZE] = {{0}};

    for (unsigned i = 0u; i < sizeof(expected); ++i) {
        ((uint8_t *)expected)[i] = (uint8_t)(i * 19u + 3u);
    }
    make_receive_batch(expected, 2u);

    TEST_ASSERT_TRUE(pf_sdio_pio_host_read_blocks(&host, 17u, true,
                                                   &received[0][0], 2u));
    TEST_ASSERT_EQUAL_MEMORY(expected, received, sizeof(expected));
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_command_count);
    TEST_ASSERT_EQUAL_UINT8(12u, pf_test_sdio_last_command);
}

void test_production_cmd18_reader_stops_after_crc_failure_and_preserves_unresolved_state(void)
{
    pf_test_sdio_pio_t pio = {0};
    pf_sdio_pio_host_t host = {
        .pio = &pio,
        .command_sm = 0u,
        .data_sm = 1u,
        .dma_channel = 3u,
        .data_rx_program_offset = 4u,
        .data_rx_program_loaded = true,
        .data_rx_multiblock_supported = true,
        .current_clock_hz = 25000000u,
        .clock_div_ticks = 6u * 256u
    };
    uint8_t expected[2][PF_SD_CARD_BLOCK_SIZE] = {{0}};
    uint8_t received[2][PF_SD_CARD_BLOCK_SIZE] = {{0}};

    make_receive_batch(expected, 2u);
    pf_test_sdio_batch_rx[PF_SDIO_PIO_IMAGE_SIZE + PF_SD_CARD_BLOCK_SIZE] ^= 1u;

    TEST_ASSERT_FALSE(pf_sdio_pio_host_read_blocks(&host, 17u, true,
                                                    &received[0][0], 2u));
    TEST_ASSERT_FALSE(host.read_stream_unresolved);
    TEST_ASSERT_EQUAL_UINT8(12u, pf_test_sdio_last_command);
    TEST_ASSERT_EQUAL_UINT(2u, pf_test_sdio_command_count);

    setUp();
    host.read_stream_unresolved = false;
    make_receive_batch(expected, 2u);
    pf_test_sdio_command_error = 12u;
    TEST_ASSERT_FALSE(pf_sdio_pio_host_read_blocks(&host, 17u, true,
                                                    &received[0][0], 2u));
    TEST_ASSERT_TRUE(host.read_stream_unresolved);
}
