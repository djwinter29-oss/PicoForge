/** @file SD protocol adapter over the native PIO wire transport. */
#include "sd_card_native_host.h"
#include "sd_card_protocol.h"
#include <string.h>
#include "pico/stdlib.h"

#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT PF_SDIO_PIO_TRANSFER_WORDS
#define SDIO_READ_BATCH_BLOCKS 64u
#define SDIO_DATA_TIMEOUT_US 1000000u

/* ponytail: at most 64 sectors per DMA batch (32 KiB payload). Decode completed
 * wire blocks while later blocks arrive; use chained ping-pong DMA if this ceiling becomes limiting. */
static uint32_t read_wire_buffer[SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS];
static pf_sdio_pio_wait_hook_t data_wait_hook;

static void set_error(pf_sdio_pio_host_t *host, pf_sd_card_detail_error_t error)
{
    if (host != NULL) {
        host->last_error = error;
    }
}

/** @brief Map physical failures into the SD adapter's existing diagnostic contract. */
static void transport_error(void *context, pf_storage_sdio_error_t error, pf_storage_sdio_phase_t phase)
{
    pf_sdio_pio_host_t *host = context;
    bool timeout = (error == PF_STORAGE_SDIO_ERROR_PIO_TIMEOUT) ||
                   (error == PF_STORAGE_SDIO_ERROR_DMA_TIMEOUT) ||
                   (error == PF_STORAGE_SDIO_ERROR_BUSY_TIMEOUT);
    if (timeout) {
        pf_sd_card_profile_event(PF_SD_CARD_PROFILE_TIMEOUT);
        if (phase == PF_STORAGE_SDIO_PHASE_COMMAND) {
            ++host->errors.command_timeouts;
        } else if (phase == PF_STORAGE_SDIO_PHASE_DATA) {
            ++host->errors.data_timeouts;
        }
    }
    switch (error) {
    case PF_STORAGE_SDIO_ERROR_NONE:
        break;
    case PF_STORAGE_SDIO_ERROR_STATE:
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        break;
    case PF_STORAGE_SDIO_ERROR_PIO_TIMEOUT:
        ++host->errors.pio_timeouts;
        set_error(host, PF_SD_CARD_DETAIL_PIO_TIMEOUT);
        break;
    case PF_STORAGE_SDIO_ERROR_DMA_TIMEOUT:
        ++host->errors.dma_timeouts;
        set_error(host, PF_SD_CARD_DETAIL_DMA_TIMEOUT);
        break;
    case PF_STORAGE_SDIO_ERROR_BUSY_TIMEOUT:
        ++host->errors.busy_timeouts;
        set_error(host, PF_SD_CARD_DETAIL_BUSY_TIMEOUT);
        break;
    case PF_STORAGE_SDIO_ERROR_TX_STALL:
        ++host->errors.tx_stalls;
        set_error(host, PF_SD_CARD_DETAIL_TX_STALL);
        break;
    case PF_STORAGE_SDIO_ERROR_RX_STALL:
        ++host->errors.rx_stalls;
        set_error(host, PF_SD_CARD_DETAIL_RX_STALL);
        break;
    }
}

/** @brief Bind callbacks also for host-test fixtures that construct a host without init. */
static pf_storage_sdio_pio_t *wire(pf_sdio_pio_host_t *host)
{
    host->transport.error_hook = transport_error;
    host->transport.hook_context = host;
    host->transport.idle_hook = pf_sd_card_idle;
    host->transport.wait_hook = data_wait_hook;
    return &host->transport;
}

static bool parse_host_response(pf_sdio_pio_host_t *host, uint8_t command,
                                const uint8_t response[6], uint32_t *argument)
{
    pf_sd_card_detail_error_t error = pf_sdio_pio_decode_response(command, response, argument, true);
    if (error == PF_SD_CARD_DETAIL_NONE) {
        return true;
    }
    if (error == PF_SD_CARD_DETAIL_RESPONSE_CRC) {
        ++host->errors.command_crc_errors;
    }
    set_error(host, error);
    return false;
}

static bool response_status_ok(pf_sdio_pio_host_t *host, uint32_t argument)
{
    if ((argument & PF_SDIO_PIO_R1_ERROR_MASK) == 0u) {
        return true;
    }
    set_error(host, PF_SD_CARD_DETAIL_CARD_STATUS);
    return false;
}

void pf_sdio_pio_set_wait_hook(pf_sdio_pio_wait_hook_t hook)
{
    data_wait_hook = hook;
}

bool pf_sdio_pio_host_init(pf_sdio_pio_host_t *host, const pf_sdio_pio_config_t *config)
{
    if ((host == NULL) || (host->transport.pio != NULL) || (config == NULL)) {
        return false;
    }
    pf_storage_sdio_pio_config_t pins = {
        .pio = config->pio, .pins = config->pins,
    };
    host->transport.config = pins;
    pf_storage_bus_t bus = pf_storage_sdio_pio_bus(&host->transport);
    if (pf_storage_bus_init(&bus) != PF_STORAGE_BUS_OK) {
        return false;
    }
    pf_sd_card_profile_set_clock(time_us_32);
    host->scr_stage = PF_SDIO_PIO_INIT_STAGE_NONE;
    host->write_stream_next_sector = 0u;
    host->bus_width = 1u;
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    memset(&host->errors, 0, sizeof(host->errors));
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    host->data_rx_multiblock_supported = true;
    (void)wire(host);
    return true;
}

void pf_sdio_pio_host_deinit(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (host->transport.pio == NULL)) {
        return;
    }
    pf_storage_bus_t bus = pf_storage_sdio_pio_bus(wire(host));
    pf_storage_bus_deinit(&bus);
    memset(host, 0, sizeof(*host));
}

void pf_sdio_pio_host_abort(pf_sdio_pio_host_t *host)
{
    if (host != NULL) {
        pf_sd_card_detail_error_t previous_error = host->last_error;
        pf_storage_bus_t bus = pf_storage_sdio_pio_bus(wire(host));
        pf_storage_bus_abort(&bus);
        if (previous_error != PF_SD_CARD_DETAIL_NONE) {
            host->last_error = previous_error;
        }
    }
}

bool pf_sdio_pio_host_set_clock(pf_sdio_pio_host_t *host, uint32_t clock_hz)
{
    if ((host == NULL) || host->write_stream_open || host->read_stream_unresolved) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    pf_storage_bus_t bus = pf_storage_sdio_pio_bus(wire(host));
    uint32_t actual;
    return pf_storage_bus_set_clock(&bus, clock_hz, &actual) == PF_STORAGE_BUS_OK;
}

bool pf_sdio_pio_host_prepare_initialization(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || host->transport.mode_restore_failed) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    if ((host->write_stream_open || host->read_stream_unresolved) && !pf_sdio_pio_host_recover(host)) {
        return false;
    }
    return pf_storage_sdio_pio_prepare_80_clocks(wire(host));
}

bool pf_sdio_pio_host_set_bus_width(pf_sdio_pio_host_t *host, uint8_t width)
{
    if ((host == NULL) || (host->transport.pio == NULL) || ((width != 1u) && (width != 4u)) ||
        host->write_stream_open || host->read_stream_unresolved ||
        ((host->transport.clock_owner != PF_SDIO_PIO_CLOCK_UNOWNED) &&
         (host->transport.clock_owner != PF_SDIO_PIO_CLOCK_COMMAND))) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    host->bus_width = width;
    return true;
}

bool pf_sdio_pio_host_command(pf_sdio_pio_host_t *host, uint8_t command, uint32_t argument,
    uint16_t response_bits, uint32_t *response_words, size_t response_word_capacity)
{
    uint8_t frame[6];
    if ((host == NULL) || (host->read_stream_unresolved && (command != 12u)) ||
        !pf_sdio_pio_build_command(command, argument, frame)) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    uint32_t profile_start = pf_sd_card_profile_begin();
    bool ok = pf_storage_sdio_pio_command(wire(host), frame, response_bits, response_words, response_word_capacity);
    pf_sd_card_profile_end(PF_SD_CARD_PROFILE_COMMAND, profile_start);
    return ok;
}

bool pf_sdio_pio_host_command_bytes(pf_sdio_pio_host_t *host, uint8_t command, uint32_t argument,
    uint16_t response_bits, uint8_t *response, size_t response_capacity)
{
    uint32_t words[5] = {0u};
    size_t response_size = (response_bits + 7u) / 8u;
    size_t count = (response_bits + 31u) / 32u;
    if ((response_size > response_capacity) || ((response_size > 0u) && (response == NULL)) ||
        (count > sizeof(words) / sizeof(words[0])) ||
        !pf_sdio_pio_host_command(host, command, argument, response_bits, words, count)) {
        return false;
    }
    return pf_sdio_pio_response_words_to_bytes(words, response_bits, response, response_capacity);
}

static bool read_data_block(pf_sdio_pio_host_t *host, uint8_t command,
                            uint32_t argument, uint8_t *data, size_t size)
{
    uint32_t image[PF_SDIO_PIO_TRANSFER_WORDS];
    uint8_t response[6];
    uint32_t response_argument;
    bool success = false;
    if ((host == NULL) || (host->bus_width != 4u)) {
        set_error(host, PF_SD_CARD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    if (((data == NULL) && (size != PF_SD_CARD_BLOCK_SIZE)) || (size == 0u) ||
        (size > PF_SD_CARD_BLOCK_SIZE) || ((size & 3u) != 0u) ||
        !host->transport.data_rx_program_loaded || host->read_stream_unresolved) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    uint32_t bytes = (uint32_t)size + 8u;
    if (!pf_storage_sdio_pio_rx_start(wire(host), image, bytes / 4u, bytes * 2u, 1u)) {
        return false;
    }
    if (!pf_sdio_pio_host_command_bytes(host, command, argument, 48u, response, sizeof(response)) ||
        !parse_host_response(host, command, response, &response_argument) ||
        !response_status_ok(host, response_argument) ||
        !pf_storage_sdio_pio_rx_finish(wire(host), SDIO_DATA_TIMEOUT_US)) {
        goto cleanup;
    }
    success = data != NULL
        ? pf_sdio_pio_decode_data_block_sized((const uint8_t *)image, data, size)
        : pf_sdio_pio_verify_data_block((const uint8_t *)image);
    if (!success) {
        ++host->errors.data_crc_errors;
        set_error(host, PF_SD_CARD_DETAIL_DATA_CRC);
    }
cleanup:
    pf_storage_sdio_pio_rx_cleanup(wire(host));
    return success;
}

bool pf_sdio_pio_host_read_sector(pf_sdio_pio_host_t *host, uint32_t sector,
    bool block_addressed, uint8_t block[512])
{
    uint32_t argument;
    return pf_sd_card_block_argument(sector, block_addressed, &argument) &&
           read_data_block(host, 17u, argument, block, PF_SD_CARD_BLOCK_SIZE);
}

bool pf_sdio_pio_host_read_scr(pf_sdio_pio_host_t *host, uint32_t relative_card_address, uint8_t scr[8])
{
    enum { SCR_DATA_BITS = 80u, SCR_DMA_WORDS = SCR_DATA_BITS / 8u };
    uint32_t image[SCR_DMA_WORDS];
    uint8_t response[6];
    uint32_t argument;
    uint16_t received_crc = 0u;
    bool success = false;
    if (host != NULL) {
        host->scr_stage = PF_SDIO_PIO_INIT_STAGE_SCR;
    }
    if ((host == NULL) || (scr == NULL) || !host->transport.data_rx_program_loaded) {
        return false;
    }
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    if (!pf_storage_sdio_pio_rx_start(wire(host), image, SCR_DMA_WORDS, SCR_DATA_BITS, 1u)) {
        return false;
    }
    host->scr_stage = PF_SDIO_PIO_INIT_STAGE_SCR_PREFIX;
    if (!pf_sdio_pio_host_command_bytes(host, 55u, relative_card_address, 48u, response, sizeof(response)) ||
        !parse_host_response(host, 55u, response, &argument) || !response_status_ok(host, argument)) {
        goto cleanup;
    }
    host->scr_stage = PF_SDIO_PIO_INIT_STAGE_SCR_RESPONSE;
    if (!pf_sdio_pio_host_command_bytes(host, 51u, 0u, 48u, response, sizeof(response)) ||
        !parse_host_response(host, 51u, response, &argument) || !response_status_ok(host, argument)) {
        goto cleanup;
    }
    host->scr_stage = PF_SDIO_PIO_INIT_STAGE_SCR_DATA;
    if (!pf_storage_sdio_pio_rx_finish(wire(host), SDIO_DATA_TIMEOUT_US)) {
        goto cleanup;
    }
    memset(scr, 0, 8u);
    for (size_t bit_index = 0u; bit_index < SCR_DATA_BITS; bit_index++) {
        uint8_t byte = ((const uint8_t *)image)[bit_index / 2u];
        uint8_t nibble = (bit_index & 1u) == 0u ? (uint8_t)(byte >> 4u) : (uint8_t)(byte & 0x0fu);
        uint8_t bit = nibble & 1u;
        if (bit_index < 64u) {
            scr[bit_index / 8u] = (uint8_t)((scr[bit_index / 8u] << 1u) | bit);
        } else {
            received_crc = (uint16_t)((received_crc << 1u) | bit);
        }
    }
    host->scr_stage = PF_SDIO_PIO_INIT_STAGE_SCR_CRC;
    success = received_crc == pf_sd_card_crc16(scr, 8u);
    if (success) {
        host->scr_stage = PF_SDIO_PIO_INIT_STAGE_NONE;
    } else {
        ++host->errors.data_crc_errors;
        set_error(host, PF_SD_CARD_DETAIL_DATA_CRC);
    }
cleanup:
    pf_storage_sdio_pio_rx_cleanup(wire(host));
    return success;
}

bool pf_sdio_pio_host_read_switch_status(pf_sdio_pio_host_t *host, uint32_t argument,
    uint8_t status[PF_SDIO_PIO_SWITCH_STATUS_SIZE])
{
    return read_data_block(host, 6u, argument, status, PF_SDIO_PIO_SWITCH_STATUS_SIZE);
}

static bool stop_multiblock(pf_sdio_pio_host_t *host)
{
    uint8_t response[6];
    uint32_t argument;
    uint32_t profile_start = pf_sd_card_profile_begin();
    bool ok = pf_sdio_pio_host_command_bytes(host, 12u, 0u, 48u, response, sizeof(response)) &&
              parse_host_response(host, 12u, response, &argument) && response_status_ok(host, argument);
    if (ok) {
        uint32_t busy_start = pf_sd_card_profile_begin();
        ok = pf_storage_sdio_pio_wait_ready(wire(host), SDIO_DATA_TIMEOUT_US);
        pf_sd_card_profile_end(PF_SD_CARD_PROFILE_BUSY, busy_start);
    }
    pf_sd_card_profile_end(PF_SD_CARD_PROFILE_STOP, profile_start);
    return ok;
}

static bool read_single_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
    bool block_addressed, uint8_t *blocks, uint32_t count)
{
    while (count-- > 0u) {
        if (!pf_sdio_pio_host_read_sector(host, first_sector++, block_addressed, blocks)) {
            return false;
        }
        if (blocks != NULL) {
            blocks += PF_SD_CARD_BLOCK_SIZE;
        }
    }
    return true;
}

/** @brief CRC-decode only complete DMA blocks while later blocks are still arriving. */
static bool decode_completed_blocks(pf_sdio_pio_host_t *host, uint8_t *blocks,
    uint32_t batch_words, uint32_t *decoded_count)
{
    uint32_t words;
    if (!pf_storage_sdio_pio_rx_progress(wire(host), read_wire_buffer, batch_words, &words)) {
        return false;
    }
    uint32_t completed = words / SDIO_DATA_WORD_COUNT;
    while (*decoded_count < completed) {
        uint8_t *image = (uint8_t *)&read_wire_buffer[*decoded_count * SDIO_DATA_WORD_COUNT];
        bool valid = blocks != NULL
            ? pf_sdio_pio_decode_data_block(image, blocks + *decoded_count * PF_SD_CARD_BLOCK_SIZE)
            : pf_sdio_pio_verify_data_block(image);
        if (!valid) {
            ++host->errors.data_crc_errors;
            set_error(host, PF_SD_CARD_DETAIL_DATA_CRC);
            return false;
        }
        ++*decoded_count;
    }
    return true;
}

bool pf_sdio_pio_host_read_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
    bool block_addressed, uint8_t *blocks, uint32_t block_count)
{
    if ((host == NULL) || (host->transport.pio == NULL) ||
        !host->transport.data_rx_program_loaded || host->read_stream_unresolved) {
        set_error(host, PF_SD_CARD_DETAIL_HOST_STATE);
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_CARD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    if (block_count == 0u) {
        return true;
    }
    if (!host->data_rx_multiblock_supported || (block_count == 1u)) {
        return read_single_blocks(host, first_sector, block_addressed, blocks, block_count);
    }
    while (block_count > 0u) {
        uint32_t argument, response_argument;
        uint8_t response[6];
        uint32_t count = block_count > SDIO_READ_BATCH_BLOCKS ? SDIO_READ_BATCH_BLOCKS : block_count;
        uint32_t batch_words = SDIO_DATA_WORD_COUNT * count;
        uint32_t decoded = 0u;
        bool complete = false, crc_failed = false, wire_failed = false;
        if (!pf_sd_card_block_argument(first_sector, block_addressed, &argument)) {
            return false;
        }
        if (!pf_storage_sdio_pio_rx_start(wire(host), read_wire_buffer, batch_words, SDIO_DATA_NIBBLE_COUNT, count)) {
            return false;
        }
        /* CMD18 may reach the wire even when its response fails. Stop every attempt. */
        bool command_ok = pf_sdio_pio_host_command_bytes(host, 18u, argument, 48u, response, sizeof(response));
        host->read_stream_unresolved = true;
        bool response_valid = command_ok && parse_host_response(host, 18u, response, &response_argument);
        bool response_ok = response_valid && ((response_argument & PF_SDIO_PIO_R1_ERROR_MASK) == 0u);
        if (response_valid && !response_ok) {
            set_error(host, PF_SD_CARD_DETAIL_CARD_STATUS);
        }
        if (response_ok) {
            uint32_t start = time_us_32();
            uint32_t profile_start = pf_sd_card_profile_begin();
            for (;;) {
                if (!pf_storage_sdio_pio_rx_poll(wire(host), start, SDIO_DATA_TIMEOUT_US, &complete)) {
                    wire_failed = true;
                    break;
                }
                if (complete) {
                    break;
                }
                if (!crc_failed && !decode_completed_blocks(host, blocks, batch_words, &decoded)) {
                    crc_failed = true;
                }
                pf_storage_sdio_pio_rx_wait(wire(host));
            }
            pf_sd_card_profile_end(PF_SD_CARD_PROFILE_DATA, profile_start);
            if (complete && !crc_failed &&
                (!decode_completed_blocks(host, blocks, batch_words, &decoded) || (decoded != count))) {
                crc_failed = true;
            }
        }
        pf_storage_sdio_pio_rx_cleanup(wire(host));
        bool stop_ok = stop_multiblock(host);
        host->read_stream_unresolved = !stop_ok;
        if (!stop_ok || !response_valid || crc_failed || wire_failed) {
            return false;
        }
        if (!complete) {
            pf_sd_card_profile_event(PF_SD_CARD_PROFILE_BATCH_FALLBACK);
            host->data_rx_multiblock_supported = false;
            return read_single_blocks(host, first_sector, block_addressed, blocks, block_count);
        }
        if (blocks != NULL) {
            blocks += count * PF_SD_CARD_BLOCK_SIZE;
        }
        first_sector += count;
        block_count -= count;
    }
    return true;
}

bool pf_sdio_pio_host_write_sector(pf_sdio_pio_host_t *host, uint32_t sector,
    bool block_addressed, const uint8_t block[512])
{
    uint32_t image[PF_SDIO_PIO_TRANSFER_WORDS], argument, response_argument, token;
    uint8_t response[6];
    if ((host == NULL) || (block == NULL) || !pf_sd_card_block_argument(sector, block_addressed, &argument)) {
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_CARD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    if (!pf_sdio_pio_encode_data_block(block, (uint8_t *)image) ||
        !pf_sdio_pio_host_command_bytes(host, 24u, argument, 48u, response, sizeof(response)) ||
        !parse_host_response(host, 24u, response, &response_argument) || !response_status_ok(host, response_argument)) {
        return false;
    }
    bool success = false;
    if (pf_storage_sdio_pio_tx_prepare(wire(host)) &&
        pf_storage_sdio_pio_tx_start(wire(host), image, SDIO_DATA_WORD_COUNT, SDIO_DATA_NIBBLE_COUNT)) {
        uint32_t profile_start = pf_sd_card_profile_begin();
        success = pf_storage_sdio_pio_tx_finish(wire(host), SDIO_DATA_TIMEOUT_US, &token) &&
                  pf_sdio_pio_write_response_accepted(token);
        pf_sd_card_profile_end(PF_SD_CARD_PROFILE_DATA, profile_start);
    }
    return pf_storage_sdio_pio_restore_command(wire(host)) && success;
}

bool pf_sdio_pio_host_write_stream_continues(const pf_sdio_pio_host_t *host, uint32_t sector)
{
    return (host != NULL) && host->write_stream_open && (host->write_stream_next_sector == sector);
}

bool pf_sdio_pio_host_recover(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (!host->write_stream_open && !host->read_stream_unresolved &&
                           !host->transport.mode_restore_failed)) {
        return true;
    }
    if (!pf_storage_sdio_pio_restore_command(wire(host)) ||
        ((host->write_stream_open || host->read_stream_unresolved) && !stop_multiblock(host))) {
        return false;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    return true;
}

void pf_sdio_pio_host_forget_removed_card(pf_sdio_pio_host_t *host)
{
    if ((host == NULL) || (host->transport.pio == NULL)) {
        return;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    if (!pf_storage_sdio_pio_restore_command(wire(host))) {
        pf_sdio_pio_host_deinit(host);
    }
}

bool pf_sdio_pio_host_write_blocks(pf_sdio_pio_host_t *host, uint32_t first_sector,
    bool block_addressed, const uint8_t *blocks, uint32_t block_count)
{
    uint32_t images[2][PF_SDIO_PIO_TRANSFER_WORDS];
    uint32_t argument, response_argument;
    uint8_t response[6];
    bool continues = pf_sdio_pio_host_write_stream_continues(host, first_sector);
    if ((host == NULL) || (host->transport.pio == NULL) || (blocks == NULL) || (block_count == 0u) ||
        (!continues && (block_count < 2u)) || !pf_sd_card_block_argument(first_sector, block_addressed, &argument)) {
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_CARD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_CARD_DETAIL_NONE;
    if ((host->write_stream_open && !continues && !pf_sdio_pio_host_recover(host)) ||
        !pf_sdio_pio_encode_data_block(blocks, (uint8_t *)images[0])) {
        return false;
    }
    if (!continues) {
        bool command_ok = pf_sdio_pio_host_command_bytes(host, 25u, argument, 48u, response, sizeof(response));
        bool valid = command_ok && parse_host_response(host, 25u, response, &response_argument);
        if (!valid) {
            host->write_stream_open = true;
            host->write_stream_next_sector = first_sector;
            (void)pf_sdio_pio_host_recover(host);
            return false;
        }
        if (!response_status_ok(host, response_argument)) {
            return false;
        }
        host->write_stream_open = true;
        host->write_stream_next_sector = first_sector;
        if (!pf_storage_sdio_pio_tx_prepare(wire(host))) {
            (void)pf_sdio_pio_host_recover(host);
            return false;
        }
    }
    bool ok = true;
    for (uint32_t index = 0u; index < block_count; index++) {
        uint32_t token;
        if (!pf_storage_sdio_pio_tx_start(wire(host), images[index & 1u],
                                          SDIO_DATA_WORD_COUNT, SDIO_DATA_NIBBLE_COUNT)) {
            ok = false;
            break;
        }
        uint32_t profile_start = pf_sd_card_profile_begin();
        if ((index + 1u < block_count) &&
            !pf_sdio_pio_encode_data_block(blocks + (index + 1u) * PF_SD_CARD_BLOCK_SIZE,
                                           (uint8_t *)images[(index + 1u) & 1u])) {
            ok = false;
        }
        bool finished = pf_storage_sdio_pio_tx_finish(wire(host), SDIO_DATA_TIMEOUT_US, &token);
        pf_sd_card_profile_end(PF_SD_CARD_PROFILE_DATA, profile_start);
        if (!ok || !finished || !pf_sdio_pio_write_response_accepted(token)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        host->write_stream_next_sector = first_sector + block_count;
        return true;
    }
    (void)pf_sdio_pio_host_recover(host);
    return false;
}
