/** @file SD command/data sequencing and stream recovery over a native wire transport. */
#include "sd_native_host.h"
#include "sd_protocol.h"
#include <string.h>

#define SDIO_DATA_NIBBLE_COUNT 1040u
#define SDIO_DATA_WORD_COUNT PF_SD_NATIVE_TRANSFER_WORDS
#define SDIO_READ_BATCH_BLOCKS 64u
#define SDIO_DATA_TIMEOUT_US 1000000u
#define SDIO_COMMAND_TIMEOUT_US 100000u

/* ponytail: at most 64 sectors per DMA batch (32 KiB payload). Decode completed
 * wire blocks while later blocks arrive; use chained ping-pong DMA if this ceiling becomes limiting. */
static uint32_t read_wire_buffer[SDIO_DATA_WORD_COUNT * SDIO_READ_BATCH_BLOCKS];

static uint32_t native_profile_clock(void);

static void set_error(pf_sd_native_host_t *host, pf_sd_detail_error_t error)
{
    if (host != NULL) {
        host->last_error = error;
    }
}

/** @brief Map physical failures into the SD adapter's existing diagnostic contract. */
static void transport_error(void *context, pf_storage_sd_wire_error_t error, pf_storage_sd_wire_phase_t phase)
{
    pf_sd_native_host_t *host = context;
    bool timeout = (error == PF_STORAGE_SD_WIRE_ERROR_PIO_TIMEOUT) ||
                   (error == PF_STORAGE_SD_WIRE_ERROR_DMA_TIMEOUT) ||
                   (error == PF_STORAGE_SD_WIRE_ERROR_BUSY_TIMEOUT);
    if (timeout) {
        pf_sd_profile_event(PF_SD_PROFILE_TIMEOUT);
        if (phase == PF_STORAGE_SD_WIRE_PHASE_COMMAND) {
            ++host->errors.command_timeouts;
        } else if (phase == PF_STORAGE_SD_WIRE_PHASE_DATA) {
            ++host->errors.data_timeouts;
        }
    }
    switch (error) {
    case PF_STORAGE_SD_WIRE_ERROR_NONE:
        break;
    case PF_STORAGE_SD_WIRE_ERROR_STATE:
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        break;
    case PF_STORAGE_SD_WIRE_ERROR_PIO_TIMEOUT:
        ++host->errors.pio_timeouts;
        set_error(host, PF_SD_DETAIL_PIO_TIMEOUT);
        break;
    case PF_STORAGE_SD_WIRE_ERROR_DMA_TIMEOUT:
        ++host->errors.dma_timeouts;
        set_error(host, PF_SD_DETAIL_DMA_TIMEOUT);
        break;
    case PF_STORAGE_SD_WIRE_ERROR_BUSY_TIMEOUT:
        ++host->errors.busy_timeouts;
        set_error(host, PF_SD_DETAIL_BUSY_TIMEOUT);
        break;
    case PF_STORAGE_SD_WIRE_ERROR_TX_STALL:
        ++host->errors.tx_stalls;
        set_error(host, PF_SD_DETAIL_TX_STALL);
        break;
    case PF_STORAGE_SD_WIRE_ERROR_RX_STALL:
        ++host->errors.rx_stalls;
        set_error(host, PF_SD_DETAIL_RX_STALL);
        break;
    }
}

static const pf_storage_bus_t *profile_transport;

static uint32_t native_profile_clock(void)
{
    return (uint32_t)pf_storage_bus_time_us(profile_transport);
}

bool pf_sd_native_host_init(pf_sd_native_host_t *host, const pf_sd_config_t *config)
{
    if ((host == NULL) || pf_storage_sd_wire_state(&host->transport).ready || config == NULL ||
        !pf_storage_sd_wire_valid(&config->transport)) {
        return false;
    }
    host->transport = config->transport;
    if (pf_storage_bus_init(&host->transport) != PF_STORAGE_BUS_OK) {
        return false;
    }
    if (!pf_storage_sd_wire_hooks(&host->transport, transport_error, host, pf_sd_idle)) {
        pf_storage_bus_deinit(&host->transport);
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    profile_transport = &host->transport;
    pf_sd_profile_set_clock(native_profile_clock);
    host->scr_stage = PF_SD_NATIVE_INIT_STAGE_NONE;
    host->write_stream_next_sector = 0u;
    host->bus_width = 1u;
    host->last_error = PF_SD_DETAIL_NONE;
    memset(&host->errors, 0, sizeof(host->errors));
    memset(host->last_response_words, 0, sizeof(host->last_response_words));
    host->last_response_word_count = 0u;
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    host->data_rx_multiblock_supported = true;
    return true;
}

void pf_sd_native_host_deinit(pf_sd_native_host_t *host)
{
    if (host == NULL) {
        return;
    }
    pf_storage_bus_deinit(&host->transport);
    if (profile_transport == &host->transport) {
        pf_sd_profile_set_clock(NULL);
        profile_transport = NULL;
    }
    memset(host, 0, sizeof(*host));
}

void pf_sd_native_host_abort(pf_sd_native_host_t *host)
{
    if (host != NULL) {
        pf_sd_detail_error_t previous_error = host->last_error;
        pf_storage_bus_abort(&host->transport);
        if (previous_error != PF_SD_DETAIL_NONE) {
            host->last_error = previous_error;
        }
    }
}

bool pf_sd_native_host_set_clock(pf_sd_native_host_t *host, uint32_t clock_hz)
{
    if ((host == NULL) || host->write_stream_open || host->read_stream_unresolved) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    uint32_t actual;
    return pf_storage_bus_set_clock(&host->transport, clock_hz, &actual) == PF_STORAGE_BUS_OK;
}

bool pf_sd_native_host_prepare_initialization(pf_sd_native_host_t *host)
{
    if ((host == NULL) || !pf_storage_sd_wire_state(&host->transport).healthy) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    if ((host->write_stream_open || host->read_stream_unresolved) && !pf_sd_native_host_recover(host)) {
        return false;
    }
    if (!pf_sd_native_host_set_clock(host, 400000u) ||
        !pf_storage_sd_wire_clocks(&host->transport, 80u, SDIO_COMMAND_TIMEOUT_US)) {
        return false;
    }
    host->bus_width = 1u;
    return true;
}

bool pf_sd_native_host_set_bus_width(pf_sd_native_host_t *host, uint8_t width)
{
    if ((host == NULL) || !pf_storage_sd_wire_state(&host->transport).healthy || ((width != 1u) && (width != 4u)) ||
        host->write_stream_open || host->read_stream_unresolved) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    host->bus_width = width;
    return true;
}

bool pf_sd_native_host_command_bytes(pf_sd_native_host_t *host, uint8_t command, uint32_t argument,
    uint16_t response_bits, uint8_t *response, size_t response_capacity)
{
    uint8_t frame[6];
    if ((host == NULL) || (host->read_stream_unresolved && (command != 12u)) ||
        response_bits > 136u || response_bits % 8u != 0u || response_capacity < response_bits / 8u ||
        (response_bits != 0u && response == NULL) ||
        !pf_sd_build_command(command, argument, frame)) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    uint32_t profile_start = pf_sd_profile_begin();
    size_t received = 0u;
    bool ok = pf_storage_sd_wire_exchange(&host->transport, frame, sizeof(frame), response_bits,
                                         response, response_capacity, &received, SDIO_COMMAND_TIMEOUT_US);
    pf_sd_profile_end(PF_SD_PROFILE_COMMAND, profile_start);
    memset(host->last_response_words, 0, sizeof(host->last_response_words));
    host->last_response_word_count = (received + 3u) / 4u;
    for (size_t index = 0u; index < received; ++index) {
        uint32_t *word = &host->last_response_words[index / 4u];
        *word = (*word << 8u) | response[index];
    }
    return ok && received == response_bits / 8u;
}

static bool read_data_block(pf_sd_native_host_t *host, uint8_t command,
                            uint32_t argument, uint8_t *data, size_t size)
{
    uint32_t image[PF_SD_NATIVE_TRANSFER_WORDS];
    uint8_t response[6];
    uint32_t response_argument;
    bool success = false;
    if ((host == NULL) || (host->bus_width != 4u)) {
        set_error(host, PF_SD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_DETAIL_NONE;
    if (((data == NULL) && (size != PF_SD_BLOCK_SIZE)) || (size == 0u) ||
        (size > PF_SD_BLOCK_SIZE) || ((size & 3u) != 0u) ||
        host->read_stream_unresolved) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    uint32_t bytes = (uint32_t)size + 8u;
    if (!pf_storage_sd_wire_rx_start(&host->transport, image, bytes / 4u, bytes * 2u, 1u)) {
        return false;
    }
    if (!pf_sd_native_host_command_bytes(host, command, argument, 48u, response, sizeof(response)) ||
        !pf_sd_native_host_parse_response(host, command, response, &response_argument) ||
        !pf_sd_native_host_response_status_ok(host, response_argument, PF_SD_NATIVE_R1_ERROR_MASK) ||
        !pf_storage_sd_wire_rx_finish(&host->transport, SDIO_DATA_TIMEOUT_US)) {
        goto cleanup;
    }
    success = data != NULL
        ? pf_sd_native_decode_data_block_sized((const uint8_t *)image, data, size)
        : pf_sd_native_verify_data_block((const uint8_t *)image);
    if (!success) {
        ++host->errors.data_crc_errors;
        set_error(host, PF_SD_DETAIL_DATA_CRC);
    }
cleanup:
    pf_storage_sd_wire_rx_cancel(&host->transport);
    return success;
}

bool pf_sd_native_host_read_sector(pf_sd_native_host_t *host, uint32_t sector,
    uint8_t block[512])
{
    return read_data_block(host, 17u, sector, block, PF_SD_BLOCK_SIZE);
}

bool pf_sd_native_host_read_scr(pf_sd_native_host_t *host, uint32_t relative_card_address, uint8_t scr[8])
{
    enum { SCR_DATA_BITS = 80u, SCR_DMA_WORDS = SCR_DATA_BITS / 8u };
    uint32_t image[SCR_DMA_WORDS];
    uint8_t response[6];
    uint32_t argument;
    uint16_t received_crc = 0u;
    bool success = false;
    if (host != NULL) {
        host->scr_stage = PF_SD_NATIVE_INIT_STAGE_SCR;
    }
    if ((host == NULL) || (scr == NULL)) {
        return false;
    }
    host->last_error = PF_SD_DETAIL_NONE;
    if (!pf_storage_sd_wire_rx_start(&host->transport, image, SCR_DMA_WORDS, SCR_DATA_BITS, 1u)) {
        return false;
    }
    host->scr_stage = PF_SD_NATIVE_INIT_STAGE_SCR_PREFIX;
    if (!pf_sd_native_host_command_bytes(host, 55u, relative_card_address, 48u, response, sizeof(response)) ||
        !pf_sd_native_host_parse_response(host, 55u, response, &argument) ||
        !pf_sd_native_host_response_status_ok(host, argument, PF_SD_NATIVE_R1_ERROR_MASK)) {
        goto cleanup;
    }
    host->scr_stage = PF_SD_NATIVE_INIT_STAGE_SCR_RESPONSE;
    if (!pf_sd_native_host_command_bytes(host, 51u, 0u, 48u, response, sizeof(response)) ||
        !pf_sd_native_host_parse_response(host, 51u, response, &argument) ||
        !pf_sd_native_host_response_status_ok(host, argument, PF_SD_NATIVE_R1_ERROR_MASK)) {
        goto cleanup;
    }
    host->scr_stage = PF_SD_NATIVE_INIT_STAGE_SCR_DATA;
    if (!pf_storage_sd_wire_rx_finish(&host->transport, SDIO_DATA_TIMEOUT_US)) {
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
    host->scr_stage = PF_SD_NATIVE_INIT_STAGE_SCR_CRC;
    success = received_crc == pf_sd_crc16(scr, 8u);
    if (success) {
        host->scr_stage = PF_SD_NATIVE_INIT_STAGE_NONE;
    } else {
        ++host->errors.data_crc_errors;
        set_error(host, PF_SD_DETAIL_DATA_CRC);
    }
cleanup:
    pf_storage_sd_wire_rx_cancel(&host->transport);
    return success;
}

bool pf_sd_native_host_read_switch_status(pf_sd_native_host_t *host, uint32_t argument,
    uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE])
{
    return read_data_block(host, 6u, argument, status, PF_SD_NATIVE_SWITCH_STATUS_SIZE);
}

static bool stop_multiblock(pf_sd_native_host_t *host)
{
    uint8_t response[6];
    uint32_t argument;
    uint32_t profile_start = pf_sd_profile_begin();
    bool ok = pf_sd_native_host_command_bytes(host, 12u, 0u, 48u, response, sizeof(response)) &&
              pf_sd_native_host_parse_response(host, 12u, response, &argument) &&
              pf_sd_native_host_response_status_ok(host, argument, PF_SD_NATIVE_R1_ERROR_MASK);
    if (ok) {
        uint32_t busy_start = pf_sd_profile_begin();
        ok = pf_storage_sd_wire_wait_high(&host->transport, SDIO_DATA_TIMEOUT_US);
        pf_sd_profile_end(PF_SD_PROFILE_BUSY, busy_start);
    }
    pf_sd_profile_end(PF_SD_PROFILE_STOP, profile_start);
    return ok;
}

static bool read_single_blocks(pf_sd_native_host_t *host, uint32_t first_sector,
    uint8_t *blocks, uint32_t count)
{
    while (count-- > 0u) {
        if (!pf_sd_native_host_read_sector(host, first_sector++, blocks)) {
            return false;
        }
        if (blocks != NULL) {
            blocks += PF_SD_BLOCK_SIZE;
        }
    }
    return true;
}

/** @brief CRC-decode only complete DMA blocks while later blocks are still arriving. */
static bool decode_completed_blocks(pf_sd_native_host_t *host, uint8_t *blocks,
    uint32_t batch_words, uint32_t *decoded_count)
{
    uint32_t words;
    if (!pf_storage_sd_wire_rx_progress(&host->transport, read_wire_buffer, batch_words, &words)) {
        return false;
    }
    uint32_t completed = words / SDIO_DATA_WORD_COUNT;
    while (*decoded_count < completed) {
        uint8_t *image = (uint8_t *)&read_wire_buffer[*decoded_count * SDIO_DATA_WORD_COUNT];
        bool valid = blocks != NULL
            ? pf_sd_native_decode_data_block(image, blocks + *decoded_count * PF_SD_BLOCK_SIZE)
            : pf_sd_native_verify_data_block(image);
        if (!valid) {
            ++host->errors.data_crc_errors;
            set_error(host, PF_SD_DETAIL_DATA_CRC);
            return false;
        }
        ++*decoded_count;
    }
    return true;
}

bool pf_sd_native_host_read_blocks(pf_sd_native_host_t *host, uint32_t first_sector,
    uint8_t *blocks, uint32_t block_count)
{
    if ((host == NULL) || !pf_storage_sd_wire_state(&host->transport).ready ||
        host->read_stream_unresolved) {
        set_error(host, PF_SD_DETAIL_HOST_STATE);
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_DETAIL_NONE;
    if (block_count == 0u) {
        return true;
    }
    if (!host->data_rx_multiblock_supported || (block_count == 1u)) {
        return read_single_blocks(host, first_sector, blocks, block_count);
    }
    while (block_count > 0u) {
        uint32_t response_argument;
        uint8_t response[6];
        uint32_t count = block_count > SDIO_READ_BATCH_BLOCKS ? SDIO_READ_BATCH_BLOCKS : block_count;
        uint32_t batch_words = SDIO_DATA_WORD_COUNT * count;
        uint32_t decoded = 0u;
        bool complete = false, crc_failed = false, wire_failed = false;
        if (!pf_storage_sd_wire_rx_start(&host->transport, read_wire_buffer, batch_words,
                                                  SDIO_DATA_NIBBLE_COUNT, count)) {
            return false;
        }
        /* CMD18 may reach the wire even when its response fails. Stop every attempt. */
        bool command_ok = pf_sd_native_host_command_bytes(host, 18u, first_sector, 48u, response, sizeof(response));
        host->read_stream_unresolved = true;
        bool response_valid = command_ok && pf_sd_native_host_parse_response(host, 18u, response, &response_argument);
        bool response_ok = response_valid &&
            pf_sd_native_host_response_status_ok(host, response_argument, PF_SD_NATIVE_R1_ERROR_MASK);
        if (response_ok) {
            uint32_t start = (uint32_t)pf_storage_bus_time_us(&host->transport);
            uint32_t profile_start = pf_sd_profile_begin();
            for (;;) {
                if (!pf_storage_sd_wire_rx_poll(&host->transport, start, SDIO_DATA_TIMEOUT_US, &complete)) {
                    wire_failed = true;
                    break;
                }
                if (complete) {
                    break;
                }
                if (!crc_failed && !decode_completed_blocks(host, blocks, batch_words, &decoded)) {
                    crc_failed = true;
                }
                pf_storage_sd_wire_rx_wait(&host->transport);
            }
            pf_sd_profile_end(PF_SD_PROFILE_DATA, profile_start);
            if (complete && !crc_failed &&
                (!decode_completed_blocks(host, blocks, batch_words, &decoded) || (decoded != count))) {
                crc_failed = true;
            }
        }
        pf_storage_sd_wire_rx_cancel(&host->transport);
        bool stop_ok = stop_multiblock(host);
        host->read_stream_unresolved = !stop_ok;
        if (!stop_ok || !response_valid || crc_failed || wire_failed) {
            return false;
        }
        if (!complete) {
            pf_sd_profile_event(PF_SD_PROFILE_BATCH_FALLBACK);
            host->data_rx_multiblock_supported = false;
            return read_single_blocks(host, first_sector, blocks, block_count);
        }
        if (blocks != NULL) {
            blocks += count * PF_SD_BLOCK_SIZE;
        }
        first_sector += count;
        block_count -= count;
    }
    return true;
}

bool pf_sd_native_host_write_sector(pf_sd_native_host_t *host, uint32_t sector,
    const uint8_t block[512])
{
    uint32_t image[PF_SD_NATIVE_TRANSFER_WORDS], response_argument, token;
    uint8_t response[6];
    if ((host == NULL) || (block == NULL)) {
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_DETAIL_NONE;
    if (!pf_sd_native_encode_data_block(block, (uint8_t *)image) ||
        !pf_sd_native_host_command_bytes(host, 24u, sector, 48u, response, sizeof(response)) ||
        !pf_sd_native_host_parse_response(host, 24u, response, &response_argument) ||
        !pf_sd_native_host_response_status_ok(host, response_argument, PF_SD_NATIVE_R1_ERROR_MASK)) {
        return false;
    }
    bool success = false;
    if (pf_storage_sd_wire_tx_start(&host->transport, image, SDIO_DATA_WORD_COUNT, SDIO_DATA_NIBBLE_COUNT)) {
        uint32_t profile_start = pf_sd_profile_begin();
        success = pf_storage_sd_wire_tx_finish(&host->transport, SDIO_DATA_TIMEOUT_US, &token) &&
                  pf_sd_native_write_response_accepted(token);
        pf_sd_profile_end(PF_SD_PROFILE_DATA, profile_start);
    }
    return pf_storage_sd_wire_idle(&host->transport) && success;
}

bool pf_sd_native_host_write_stream_continues(const pf_sd_native_host_t *host, uint32_t sector)
{
    return (host != NULL) && host->write_stream_open && (host->write_stream_next_sector == sector);
}

bool pf_sd_native_host_recover(pf_sd_native_host_t *host)
{
    if ((host == NULL) || (!host->write_stream_open && !host->read_stream_unresolved &&
                           pf_storage_sd_wire_state(&host->transport).healthy)) {
        return true;
    }
    if (!pf_storage_sd_wire_idle(&host->transport) ||
        ((host->write_stream_open || host->read_stream_unresolved) && !stop_multiblock(host))) {
        return false;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    return true;
}

void pf_sd_native_host_forget_removed_card(pf_sd_native_host_t *host)
{
    if ((host == NULL) || !pf_storage_sd_wire_state(&host->transport).ready) {
        return;
    }
    host->write_stream_open = false;
    host->read_stream_unresolved = false;
    if (!pf_storage_sd_wire_idle(&host->transport)) {
        pf_sd_native_host_deinit(host);
    }
}

bool pf_sd_native_host_write_blocks(pf_sd_native_host_t *host, uint32_t first_sector,
    const uint8_t *blocks, uint32_t block_count)
{
    uint32_t images[2][PF_SD_NATIVE_TRANSFER_WORDS];
    uint32_t response_argument;
    uint8_t response[6];
    bool continues = pf_sd_native_host_write_stream_continues(host, first_sector);
    if ((host == NULL) || !pf_storage_sd_wire_state(&host->transport).ready || (blocks == NULL) || (block_count == 0u) ||
        (!continues && (block_count < 2u))) {
        return false;
    }
    if (host->bus_width != 4u) {
        set_error(host, PF_SD_DETAIL_FOUR_BIT);
        return false;
    }
    host->last_error = PF_SD_DETAIL_NONE;
    if ((host->write_stream_open && !continues && !pf_sd_native_host_recover(host)) ||
        !pf_sd_native_encode_data_block(blocks, (uint8_t *)images[0])) {
        return false;
    }
    if (!continues) {
        bool command_ok = pf_sd_native_host_command_bytes(host, 25u, first_sector, 48u, response, sizeof(response));
        bool valid = command_ok && pf_sd_native_host_parse_response(host, 25u, response, &response_argument);
        if (!valid) {
            host->write_stream_open = true;
            host->write_stream_next_sector = first_sector;
            (void)pf_sd_native_host_recover(host);
            return false;
        }
        if (!pf_sd_native_host_response_status_ok(host, response_argument, PF_SD_NATIVE_R1_ERROR_MASK)) {
            return false;
        }
        host->write_stream_open = true;
        host->write_stream_next_sector = first_sector;
    }
    bool ok = true;
    for (uint32_t index = 0u; index < block_count; index++) {
        uint32_t token;
        if (!pf_storage_sd_wire_tx_start(&host->transport, images[index & 1u],
                                          SDIO_DATA_WORD_COUNT, SDIO_DATA_NIBBLE_COUNT)) {
            ok = false;
            break;
        }
        uint32_t profile_start = pf_sd_profile_begin();
        if ((index + 1u < block_count) &&
            !pf_sd_native_encode_data_block(blocks + (index + 1u) * PF_SD_BLOCK_SIZE,
                                           (uint8_t *)images[(index + 1u) & 1u])) {
            ok = false;
        }
        bool finished = pf_storage_sd_wire_tx_finish(&host->transport, SDIO_DATA_TIMEOUT_US, &token);
        pf_sd_profile_end(PF_SD_PROFILE_DATA, profile_start);
        if (!ok || !finished || !pf_sd_native_write_response_accepted(token)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        host->write_stream_next_sector = first_sector + block_count;
        return true;
    }
    (void)pf_sd_native_host_recover(host);
    return false;
}
