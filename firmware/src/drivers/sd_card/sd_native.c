#include "sd_native.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sd_native_host.h"
#include "sd_protocol.h"

static pf_sd_native_host_t native_host;
static pf_sd_config_t active_config;
static bool configuration_set;
static bool card_initialized;
static pf_sd_native_init_stage_t card_init_stage;
static uint8_t card_init_response[6];
static uint8_t last_short_command;
static uint32_t last_short_argument;
static uint32_t last_short_response_argument;
static pf_sd_info_t native_info;

enum {
    SD_CMD_GO_IDLE = 0, SD_CMD_ALL_SEND_CID = 2, SD_CMD_SEND_RCA = 3,
    SD_CMD_SELECT = 7, SD_CMD_IF_CONDITION = 8,
    SD_CMD_SEND_CSD = 9, SD_CMD_STATUS = 13, SD_CMD_APP = 55,
    SD_ACMD_BUS_WIDTH = 6, SD_ACMD_OP_COND = 41,
    SD_INIT_CLOCK_HZ = 400000, SD_HIGH_CLOCK_HZ = 50000000,
    SD_INIT_TIMEOUT_US = 1000000,
};

static bool protocol_failure(pf_sd_detail_error_t error)
{
    native_host.last_error = error;
    return false;
}

static bool same_config(const pf_sd_config_t *left,
                        const pf_sd_config_t *right)
{
    return left->clock_hz == right->clock_hz &&
           pf_storage_transport_same(&left->transport, &right->transport);
}

static bool config_is_valid(const pf_sd_config_t *config)
{
    if (config == NULL || !pf_storage_native_valid(&config->transport)) {
        return false;
    }

    return (config->clock_hz == 0u || config->clock_hz == PF_SD_NATIVE_TRANSFER_CLOCK_HZ) &&
           PF_SD_NATIVE_TRANSFER_CLOCK_HZ <=
               pf_storage_native_state(&config->transport).maximum_clock_hz;
}

static bool command_response(uint8_t command, uint32_t argument, uint8_t response[6],
                             uint32_t *response_argument)
{
    memset(card_init_response, 0, sizeof(card_init_response));
    if (!pf_sd_native_host_command_bytes(&native_host, command, argument, 48u,
                                       response, 6u)) {
        return false;
    }
    memcpy(card_init_response, response, sizeof(card_init_response));
    if (!pf_sd_native_host_parse_response(&native_host, command, response, response_argument)) {
        return false;
    }
    last_short_command = command;
    last_short_argument = argument;
    last_short_response_argument = *response_argument;
    if (command == SD_CMD_IF_CONDITION) {
        return true;
    }
    uint32_t mask = command == SD_CMD_SEND_RCA ? PF_SD_NATIVE_R6_ERROR_MASK
                                               : PF_SD_NATIVE_R1_ERROR_MASK;
    return pf_sd_native_host_response_status_ok(&native_host, *response_argument, mask);
}

static bool card_detect_present(void)
{
    return pf_storage_transport_present(&active_config.transport);
}

static bool wait_card_data_ready(void)
{
    uint32_t start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);

    while (!pf_storage_native_state(&active_config.transport).data_high) {
        if (!card_detect_present()) {
            return protocol_failure(PF_SD_DETAIL_REMOVED);
        }
        if ((uint32_t)(pf_storage_transport_time_us(&active_config.transport) - start) > 1000000u) {
            ++native_host.errors.busy_timeouts;
            return protocol_failure(PF_SD_DETAIL_BUSY_TIMEOUT);
        }
        pf_sd_idle();
    }
    return true;
}

static bool transfer_ready(uint32_t card_address)
{
    uint8_t response[6];
    uint32_t status;
    return command_response(SD_CMD_STATUS, card_address, response, &status) &&
           (pf_sd_native_status_is_transfer_ready(status) ||
            protocol_failure(PF_SD_DETAIL_CARD_STATUS));
}

static bool validate_transfer(uint32_t card_address)
{
    uint8_t block[PF_SD_BLOCK_SIZE];
    if (!card_detect_present()) {
        return protocol_failure(PF_SD_DETAIL_REMOVED);
    }
    return transfer_ready(card_address) &&
           pf_sd_native_host_read_sector(&native_host, 0u, block);
}

#if PF_SD_NATIVE_HIGH_SPEED
static bool select_transfer_speed(uint32_t card_address)
{
    uint8_t status[PF_SD_NATIVE_SWITCH_STATUS_SIZE];
    native_info.high_speed_error = PF_SD_DETAIL_NONE;
    if (pf_storage_native_state(&active_config.transport).maximum_clock_hz < SD_HIGH_CLOCK_HZ) {
        native_info.high_speed_error = PF_SD_DETAIL_HOST_STATE;
        return true;
    }
    if (!pf_sd_native_host_read_switch_status(&native_host,
            PF_SD_NATIVE_CMD6_CHECK_HIGH_SPEED, status)) {
        native_info.high_speed_error = native_host.last_error;
        /* A lost CMD6 response can leave the card busy; prove continued operation. */
        return wait_card_data_ready() && validate_transfer(card_address);
    }
    if (!pf_sd_native_switch_supports_high_speed(status) ||
        pf_sd_native_switch_high_speed_busy(status)) {
        native_info.high_speed_error = PF_SD_DETAIL_HIGH_SPEED_UNSUPPORTED;
        return true;
    }
    bool switched = pf_sd_native_host_read_switch_status(&native_host,
        PF_SD_NATIVE_CMD6_SWITCH_HIGH_SPEED, status);
    if (!switched || !pf_sd_native_switch_selected_high_speed(status) ||
        pf_sd_native_switch_high_speed_busy(status)) {
        native_info.high_speed_error = PF_SD_DETAIL_HIGH_SPEED_REJECTED;
        return wait_card_data_ready() &&
               pf_sd_native_host_read_switch_status(&native_host,
                   PF_SD_NATIVE_CMD6_SWITCH_DEFAULT, status) &&
               (status[16] & 0x0fu) == 0u &&
               validate_transfer(card_address);
    }
    if (!wait_card_data_ready()) {
        return false;
    }
    native_info.high_speed = true;
    native_info.requested_clock_hz = SD_HIGH_CLOCK_HZ;
    if (pf_sd_native_host_set_clock(&native_host, SD_HIGH_CLOCK_HZ) &&
        validate_transfer(card_address)) {
        return true;
    }
    native_info.high_speed_error = native_host.last_error;
    ++native_info.high_speed_fallbacks;
    native_info.requested_clock_hz = PF_SD_NATIVE_TRANSFER_CLOCK_HZ;
    /* Default-Speed timing requires switching the card back as well as the host. */
    if (!pf_sd_native_host_set_clock(&native_host, PF_SD_NATIVE_TRANSFER_CLOCK_HZ) ||
        !wait_card_data_ready() ||
        !pf_sd_native_host_read_switch_status(&native_host,
            PF_SD_NATIVE_CMD6_SWITCH_DEFAULT, status) ||
        (status[16] & 0x0fu) != 0u || !wait_card_data_ready()) {
        return false;
    }
    native_info.high_speed = false;
    return validate_transfer(card_address);
}
#endif

static bool initialize_card(void)
{
    uint8_t response[17];
    uint8_t short_response[6];
    uint8_t csd[16];
    uint8_t scr[8];
    uint32_t response_argument;
    uint32_t ocr;
    uint32_t start;
    uint32_t card_default_transfer_hz;
    const uint32_t acmd41_argument = PF_SD_NATIVE_OCR_VOLTAGE | PF_SD_NATIVE_OCR_HCS;

    if (!card_detect_present()) {
        return protocol_failure(PF_SD_DETAIL_REMOVED);
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_IDLE;
    pf_storage_transport_delay_ms(&active_config.transport, 2u);
    if (!pf_sd_native_host_command_bytes(&native_host, SD_CMD_GO_IDLE, 0u, 0u, NULL, 0u)) {
        return false;
    }

    card_init_stage = PF_SD_NATIVE_INIT_STAGE_CMD8_RESPONSE;
    if (!command_response(SD_CMD_IF_CONDITION, PF_SD_NATIVE_R7_ARGUMENT,
                          short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_CMD8_CLASSIFY;
    if ((response_argument & PF_SD_NATIVE_R1_ILLEGAL_COMMAND) != 0u) {
        return protocol_failure(PF_SD_DETAIL_LEGACY_CARD);
    }
    if ((response_argument & PF_SD_NATIVE_R7_VOLTAGE_MASK) != PF_SD_NATIVE_R7_VOLTAGE_3V3) {
        return protocol_failure(PF_SD_DETAIL_VOLTAGE);
    }
    if (response_argument != PF_SD_NATIVE_R7_ARGUMENT) {
        return protocol_failure(PF_SD_DETAIL_R7);
    }

    card_init_stage = PF_SD_NATIVE_INIT_STAGE_OPERATING_CONDITION;
    start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);
    do {
        card_init_stage = PF_SD_NATIVE_INIT_STAGE_OPERATING_PREFIX;
        if (!card_detect_present()) {
            return protocol_failure(PF_SD_DETAIL_REMOVED);
        }
        if (!command_response(SD_CMD_APP, 0u, short_response, &response_argument)) {
            return false;
        }
        card_init_stage = PF_SD_NATIVE_INIT_STAGE_OPERATING_RESPONSE;
        memset(card_init_response, 0, sizeof(card_init_response));
        if (!pf_sd_native_host_command_bytes(&native_host, SD_ACMD_OP_COND, acmd41_argument,
                                            48u, short_response, sizeof(short_response))) {
            return false;
        }
        memcpy(card_init_response, short_response, sizeof(card_init_response));
        card_init_stage = PF_SD_NATIVE_INIT_STAGE_OPERATING_PARSE;
        if (!pf_sd_native_parse_r3_response(short_response, &ocr)) {
            return protocol_failure(PF_SD_DETAIL_OCR_CCS);
        }
        card_init_stage = PF_SD_NATIVE_INIT_STAGE_OPERATING_READY;
        native_info.ocr = ocr;
        if ((ocr & PF_SD_NATIVE_OCR_READY) != 0u) {
            break;
        }
        pf_storage_transport_delay_ms(&active_config.transport, 10u);
    } while ((uint32_t)((uint32_t)pf_storage_transport_time_us(&active_config.transport) - start) < SD_INIT_TIMEOUT_US);
    if ((ocr & PF_SD_NATIVE_OCR_READY) == 0u) {
        return protocol_failure(PF_SD_DETAIL_INIT_TIMEOUT);
    }
    if ((ocr & PF_SD_NATIVE_OCR_HCS) == 0u ||
        (ocr & PF_SD_NATIVE_OCR_VOLTAGE) == 0u) {
        return protocol_failure(PF_SD_DETAIL_OCR_CCS);
    }

    card_init_stage = PF_SD_NATIVE_INIT_STAGE_CID_RESPONSE;
    if (!pf_sd_native_host_command_bytes(&native_host, SD_CMD_ALL_SEND_CID, 0u, 136u,
                                        response, sizeof(response))) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_CID_VALIDATE;
    if (response[0] != 0x3fu) {
        return protocol_failure(PF_SD_DETAIL_RESPONSE_INDEX);
    }
    if (!pf_sd_native_validate_r2_response(response)) {
        ++native_host.errors.command_crc_errors;
        return protocol_failure(PF_SD_DETAIL_RESPONSE_CRC);
    }
    memcpy(native_info.cid, response + 1u, sizeof(native_info.cid));
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_RCA;
    if (!command_response(SD_CMD_SEND_RCA, 0u, short_response, &response_argument)) {
        return false;
    }
    uint32_t relative_card_address = response_argument & 0xffff0000u;
    if (relative_card_address == 0u) {
        return protocol_failure(PF_SD_DETAIL_CARD_STATUS);
    }
    native_info.rca = (uint16_t)(relative_card_address >> 16u);
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_CSD;
    if (!pf_sd_native_host_command_bytes(&native_host, SD_CMD_SEND_CSD, relative_card_address,
                                          136u, response, sizeof(response))) {
        return false;
    }
    if (response[0] != 0x3fu) {
        return protocol_failure(PF_SD_DETAIL_RESPONSE_INDEX);
    }
    if (!pf_sd_native_validate_r2_response(response)) {
        ++native_host.errors.command_crc_errors;
        return protocol_failure(PF_SD_DETAIL_RESPONSE_CRC);
    }
    for (size_t byte_index = 0u; byte_index < sizeof(csd); byte_index++) {
        csd[byte_index] = response[byte_index + 1u];
    }
    memcpy(native_info.csd, csd, sizeof(csd));
    if ((csd[0] >> 6u) != 1u ||
        !pf_sd_csd_sector_count(csd, &native_info.block_count)) {
        return protocol_failure(PF_SD_DETAIL_LEGACY_CARD);
    }
    if (!pf_sd_csd_transfer_hz(csd, &card_default_transfer_hz) ||
        card_default_transfer_hz < PF_SD_NATIVE_TRANSFER_CLOCK_HZ) {
        return protocol_failure(PF_SD_DETAIL_HOST_STATE);
    }

    card_init_stage = PF_SD_NATIVE_INIT_STAGE_SELECT_RESPONSE;
    if (!command_response(SD_CMD_SELECT, relative_card_address, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_SELECT_READY;
    if (!wait_card_data_ready()) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_SELECT_STATUS;
    if (!transfer_ready(relative_card_address)) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_SCR;
    if (!pf_sd_native_host_read_scr(&native_host, relative_card_address, scr)) {
        card_init_stage = native_host.scr_stage;
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_SCR_BUS_WIDTH;
    memcpy(native_info.scr, scr, sizeof(scr));
    native_info.sd_spec = scr[0] & 0x0fu;
    if (native_info.sd_spec == 2u && (scr[2] & 0x80u) != 0u) {
        native_info.sd_spec = (scr[2] & 0x04u) != 0u ? 4u : 3u;
        uint8_t spec_x = (uint8_t)(((scr[2] & 3u) << 2u) | (scr[3] >> 6u));
        if (native_info.sd_spec == 4u && spec_x != 0u) {
            native_info.sd_spec = (uint8_t)(4u + spec_x);
        }
    }
    native_info.supported_bus_widths = scr[1] & 0x0fu;
    if ((scr[0] >> 4u) != 0u || !pf_sd_scr_supports_four_bit(scr)) {
        return protocol_failure(PF_SD_DETAIL_FOUR_BIT);
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_BUS_WIDTH_PREFIX;
    if (!command_response(SD_CMD_APP, relative_card_address, short_response, &response_argument)) {
        return false;
    }
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_BUS_WIDTH;
    if (!command_response(SD_ACMD_BUS_WIDTH, PF_SD_NATIVE_BUS_WIDTH_FOUR_ARGUMENT,
                          short_response, &response_argument)) {
        return false;
    }
    if (!pf_sd_native_host_set_bus_width(&native_host, 4u)) {
        return false;
    }

    card_init_stage = PF_SD_NATIVE_INIT_STAGE_TRANSFER_CLOCK;
    if (!pf_sd_native_host_set_clock(&native_host, PF_SD_NATIVE_TRANSFER_CLOCK_HZ)) {
        return false;
    }
    native_host.data_rx_multiblock_supported = true;
    if (!validate_transfer(relative_card_address)) {
        return false;
    }
#if PF_SD_NATIVE_HIGH_SPEED
    if (!select_transfer_speed(relative_card_address)) {
        return false;
    }
#endif
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_NONE;
    return true;
}

pf_sd_native_init_stage_t pf_sd_native_get_init_stage(void)
{
    return card_init_stage;
}

void pf_sd_native_get_init_response(uint8_t response[6])
{
    if (response != NULL) {
        memcpy(response, card_init_response, sizeof(card_init_response));
    }
}

void pf_sd_native_get_last_short_response(uint8_t *command, uint32_t *argument,
                                         uint32_t *response_argument)
{
    if (command != NULL) {
        *command = last_short_command;
    }
    if (argument != NULL) {
        *argument = last_short_argument;
    }
    if (response_argument != NULL) {
        *response_argument = last_short_response_argument;
    }
}

void pf_sd_native_get_last_response_words(uint32_t words[5], size_t *word_count)
{
    if (words != NULL) {
        memcpy(words, native_host.last_response_words, sizeof(native_host.last_response_words));
    }
    if (word_count != NULL) {
        *word_count = native_host.last_response_word_count;
    }
}

pf_sd_result_t pf_sd_native_init(const pf_sd_config_t *config)
{
    if (!config_is_valid(config)) {
        return PF_SD_INVALID_ARGUMENT;
    }

    if (configuration_set) {
        if (!same_config(&active_config, config)) {
            return PF_SD_CONFIG_LOCKED;
        }
    } else {
        active_config = *config;
        configuration_set = true;
    }

    uint32_t attempts = native_info.init_attempts + 1u;
    uint32_t fallbacks = native_info.high_speed_fallbacks;
    native_info = (pf_sd_info_t) {
        .native_details_valid = true,
        .initialization_error = true,
        .requested_init_clock_hz = SD_INIT_CLOCK_HZ,
        .requested_clock_hz = PF_SD_NATIVE_TRANSFER_CLOCK_HZ,
        .init_attempts = attempts,
        .high_speed_fallbacks = fallbacks,
    };
    card_initialized = false;
    memset(card_init_response, 0, sizeof(card_init_response));
    last_short_command = 0u;
    last_short_argument = 0u;
    last_short_response_argument = 0u;
    card_init_stage = PF_SD_NATIVE_INIT_STAGE_NONE;
    native_host.last_error = PF_SD_DETAIL_NONE;
    if (!pf_storage_native_state(&native_host.transport).ready) {
        if (!pf_sd_native_host_init(&native_host, &active_config)) {
            goto failed;
        }
    }

    if (!card_detect_present()) {
        protocol_failure(PF_SD_DETAIL_REMOVED);
        goto failed;
    }
    if (!pf_sd_native_host_prepare_initialization(&native_host)) {
        goto failed;
    }
    native_info.actual_init_clock_hz = pf_storage_native_state(&native_host.transport).current_clock_hz;
    if (initialize_card()) {
        card_initialized = true;
        native_info.initialization_error = false;
        native_host.last_error = PF_SD_DETAIL_NONE;
        return PF_SD_OK;
    }
failed:
    if (native_host.last_error == PF_SD_DETAIL_NONE) {
        protocol_failure(PF_SD_DETAIL_HOST_STATE);
    }
    pf_sd_native_host_abort(&native_host);
    return PF_SD_INIT_FAILED;
}

bool pf_sd_native_is_initialized(void)
{
    if (!pf_storage_native_state(&native_host.transport).healthy ||
        (configuration_set && !card_detect_present())) {
        /* Absence or broken host mode blocks I/O without discarding protocol state. */
        card_initialized = false;
    }
    return card_initialized;
}

void pf_sd_native_confirm_card_removed(void)
{
    card_initialized = false;
    pf_sd_native_host_forget_removed_card(&native_host);
    protocol_failure(PF_SD_DETAIL_REMOVED);
}

uint32_t pf_sd_native_block_count(void)
{
    return card_initialized ? native_info.block_count : 0u;
}

static pf_sd_result_t runtime_failure(void)
{
    card_initialized = false;
    native_info.initialization_error = false;
    if (native_host.last_error == PF_SD_DETAIL_NONE) {
        protocol_failure(PF_SD_DETAIL_HOST_STATE);
    }
    pf_sd_native_host_abort(&native_host);
    return PF_SD_IO_ERROR;
}

static pf_sd_result_t transfer_blocks(uint32_t first_block, uint8_t *read_buffer,
                                           const uint8_t *write_buffer, uint32_t block_count)
{
    pf_sd_result_t result;

    if (!pf_sd_native_is_initialized()) {
        return PF_SD_NOT_READY;
    }
    if (block_count == 0u) {
        return PF_SD_OK;
    }

    result = pf_sd_validate_transfer(first_block, read_buffer != NULL ? read_buffer : write_buffer,
                                          block_count, pf_sd_native_block_count());
    if (result != PF_SD_OK) {
        return result;
    }

    bool continues_write = (write_buffer != NULL) &&
        pf_sd_native_host_write_stream_continues(&native_host, first_block);
    if (!continues_write && !pf_sd_native_host_recover(&native_host)) {
        return runtime_failure();
    }
    if ((write_buffer != NULL) && (continues_write || (block_count > 1u))) {
        bool success = pf_sd_native_host_write_blocks(&native_host, first_block, write_buffer, block_count);
        return success ? PF_SD_OK : runtime_failure();
    }
    if (read_buffer != NULL) {
        bool success = pf_sd_native_host_read_blocks(&native_host, first_block, read_buffer, block_count);
        return success ? PF_SD_OK : runtime_failure();
    }

    bool success = pf_sd_native_host_write_sector(&native_host, first_block, write_buffer);
    /* A lost command/data response or busy timeout cannot prove CMD24 complete.
     * Restoration only repairs the host; require reinitialization, never replay. */
    if (!success || !pf_storage_native_state(&native_host.transport).healthy) {
        return runtime_failure();
    }
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_native_read_blocks(uint32_t first_block, void *buffer, uint32_t block_count)
{
    return transfer_blocks(first_block, buffer, NULL, block_count);
}

pf_sd_result_t pf_sd_native_write_blocks(uint32_t first_block, const void *buffer,
                                                    uint32_t block_count)
{
    return transfer_blocks(first_block, NULL, buffer, block_count);
}

pf_sd_result_t pf_sd_native_sync(void)
{
    if (!pf_sd_native_is_initialized()) {
        return PF_SD_NOT_READY;
    }
    if (!pf_sd_native_host_recover(&native_host)) {
        return runtime_failure();
    }
    return PF_SD_OK;
}

void pf_sd_native_get_info(pf_sd_info_t *info)
{
    if (info == NULL) {
        return;
    }
    *info = native_info;
    if (!configuration_set) {
        return;
    }
    if (pf_storage_transport_presence_known(&active_config.transport)) {
        info->presence_known = true;
        info->present = card_detect_present();
    } else if (pf_sd_native_is_initialized()) {
        info->presence_known = true;
        info->present = true;
    }
    info->initialized = pf_sd_native_is_initialized();
    info->bus_width = native_host.bus_width;
    info->actual_clock_hz = pf_storage_native_state(&native_host.transport).current_clock_hz;
    info->errors = native_host.errors;
    info->detail_error = native_host.last_error;
    if (native_info.block_count != 0u) {
        info->cid_valid = true;
    }
    info->bus_clock_hz = info->initialized ? info->actual_clock_hz : 0u;
}