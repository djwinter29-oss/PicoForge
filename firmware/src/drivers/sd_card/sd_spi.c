#include "sd_spi.h"

#include <stddef.h>
#include <string.h>

#include "sd_protocol.h"
#include "sd_spi_protocol.h"

#define SD_SPI_INIT_CLOCK_HZ 400000u
#define SD_SPI_DEFAULT_CLOCK_HZ 25000000u
#define SD_SPI_COMMAND_RETRIES 10u
#define SD_SPI_RESPONSE_TIMEOUT_US 100000u
#define SD_SPI_INITIALIZATION_TIMEOUT_US 1000000u
#define SD_SPI_WRITE_TIMEOUT_US 500000u

#define SD_CMD_GO_IDLE_STATE 0u
#define SD_CMD_SEND_IF_COND 8u
#define SD_CMD_SEND_CSD 9u
#define SD_CMD_SEND_CID 10u
#define SD_CMD_SET_BLOCKLEN 16u
#define SD_CMD_READ_SINGLE_BLOCK 17u
#define SD_CMD_READ_MULTIPLE_BLOCK 18u
#define SD_CMD_WRITE_SINGLE_BLOCK 24u
#define SD_CMD_WRITE_MULTIPLE_BLOCK 25u
#define SD_CMD_STOP_TRANSMISSION 12u
#define SD_CMD_APP 55u
#define SD_CMD_READ_OCR 58u
#define SD_ACMD_SD_SEND_OP_COND 41u

#define SD_R1_IDLE_STATE 0x01u
#define SD_R1_ILLEGAL_COMMAND 0x04u
#define SD_DATA_START_BLOCK 0xfeu
#define SD_DATA_START_MULTIPLE 0xfcu
#define SD_DATA_STOP_MULTIPLE 0xfdu
#define SD_OCR_POWER_UP_STATUS (1u << 31u)
#define SD_OCR_CCS (1u << 30u)
#define SD_OCR_VOLTAGE_WINDOW 0x00ff8000u
#define SD_OCR_HCS (1u << 30u)

static pf_sd_spi_config_t active_config;
static bool configuration_set;
static bool spi_configured;
static bool card_initialized;
static bool card_block_addressed;
static uint32_t card_sector_count;
static uint32_t card_bus_clock_hz;
static uint8_t card_cid[16];
static pf_sd_spi_init_stage_t card_init_stage;
static uint8_t last_init_command;
static uint8_t last_init_response;
static bool last_init_response_received;

static bool same_config(const pf_sd_spi_config_t *left,
                        const pf_sd_spi_config_t *right)
{
    return left->clock_hz == right->clock_hz &&
           pf_storage_transport_same(&left->transport, &right->transport);
}

static bool config_is_valid(const pf_sd_spi_config_t *config)
{
    if (config == NULL || !pf_storage_transport_valid(&config->transport, PF_STORAGE_TRANSPORT_SPI)) {
        return false;
    }

    return (config->clock_hz == 0u) ||
           ((config->clock_hz >= SD_SPI_INIT_CLOCK_HZ) &&
            (config->clock_hz <= SD_SPI_DEFAULT_CLOCK_HZ));
}

static bool card_is_present(void)
{
    return pf_storage_transport_present(&active_config.transport);
}

static uint32_t spi_profile_clock(void)
{
    return (uint32_t)pf_storage_transport_time_us(&active_config.transport);
}

static bool exchange_byte_checked(uint8_t value, uint8_t *received)
{
    if (pf_storage_transport_exchange(&active_config.transport, &value, received, 1u) != PF_STORAGE_TRANSPORT_OK) {
        card_initialized = false;
        return false;
    }
    return true;
}

static void select_card(void)
{
    if (pf_storage_transport_select(&active_config.transport, true) != PF_STORAGE_TRANSPORT_OK) {
        card_initialized = false;
    }
}

static bool deselect_card(void)
{
    /* CS is high before the trailing clocks. The pinned blocking SDK returns
     * len after receiving every byte; an alternate short-return seam gives no
     * clock-completion guarantee. Invalidate and propagate, never recover/replay. */
    if (pf_storage_transport_select(&active_config.transport, false) != PF_STORAGE_TRANSPORT_OK) {
        return false;
    }
    uint8_t ignored;
    return exchange_byte_checked(0xffu, &ignored);
}

static bool wait_ready(uint32_t timeout_us)
{
    uint32_t start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);
    uint32_t profile_start = pf_sd_profile_begin();

    do {
        uint8_t value;
        if (!exchange_byte_checked(0xffu, &value)) {
            pf_sd_profile_end(PF_SD_PROFILE_BUSY, profile_start);
            return false;
        }
        if (value == 0xffu) {
            pf_sd_profile_end(PF_SD_PROFILE_BUSY, profile_start);
            return true;
        }
        pf_sd_idle();
    } while ((uint32_t)((uint32_t)pf_storage_transport_time_us(&active_config.transport) - start) < timeout_us);

    pf_sd_profile_end(PF_SD_PROFILE_BUSY, profile_start);
    pf_sd_profile_event(PF_SD_PROFILE_TIMEOUT);
    return false;
}

static bool send_command_selected(uint8_t command, uint32_t argument,
                                  uint8_t *response)
{
    uint8_t frame[6];

    if ((response == NULL) || !pf_sd_build_command(command, argument, frame)) {
        return false;
    }

    uint32_t profile_start = pf_sd_profile_begin();
    bool success = false;
    if (pf_storage_transport_exchange(&active_config.transport, frame, NULL, sizeof(frame)) != PF_STORAGE_TRANSPORT_OK) {
        goto done;
    }
    if (command == SD_CMD_STOP_TRANSMISSION) {
        uint8_t ignored;
        if (!exchange_byte_checked(0xffu, &ignored)) {
            goto done;
        }
    }
    for (uint32_t attempt = 0u; attempt < SD_SPI_COMMAND_RETRIES; attempt++) {
        uint8_t value;
        if (!exchange_byte_checked(0xffu, &value)) {
            goto done;
        }
        if ((value & 0x80u) == 0u) {
            *response = value;
            last_init_command = command;
            last_init_response = value;
            last_init_response_received = true;
            success = true;
            break;
        }
    }
done:
    pf_sd_profile_end(PF_SD_PROFILE_COMMAND, profile_start);
    return success;
}

static bool send_command(uint8_t command, uint32_t argument,
                         uint8_t *response, uint8_t *extra,
                         size_t extra_size)
{
    bool success = false;

    if (!card_is_present()) {
        return false;
    }

    select_card();
    if (wait_ready(SD_SPI_RESPONSE_TIMEOUT_US) &&
        send_command_selected(command, argument, response)) {
        success = (extra_size == 0u) ||
                  ((extra != NULL) &&
                   (pf_storage_transport_exchange(&active_config.transport, NULL, extra, extra_size) == PF_STORAGE_TRANSPORT_OK));
    }
    bool deselected = deselect_card();
    if (!deselected) {
        card_initialized = false;
    }
    return success && deselected;
}

static bool send_application_command(uint8_t command, uint32_t argument,
                                     uint8_t *response)
{
    uint8_t app_response;

    return send_command(SD_CMD_APP, 0u, &app_response, NULL, 0u) &&
           pf_sd_spi_r1_is_ready_or_idle(app_response) &&
           send_command(command, argument, response, NULL, 0u);
}

static bool read_data_block(uint8_t *data, size_t size)
{
    uint32_t start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);
    /* DATA covers token wait, payload and wire CRC; CRC measures CPU validation. */
    uint32_t profile_start = pf_sd_profile_begin();
    uint8_t token, crc_high, crc_low;
    bool received = false;

    do {
        if (!exchange_byte_checked(0xffu, &token)) {
            goto data_done;
        }
        if (pf_sd_spi_read_token_is_valid(token)) {
            break;
        }
        if (token != 0xffu) {
            goto data_done;
        }
        pf_sd_idle();
    } while ((uint32_t)((uint32_t)pf_storage_transport_time_us(&active_config.transport) - start) < SD_SPI_RESPONSE_TIMEOUT_US);
    if (!pf_sd_spi_read_token_is_valid(token)) {
        pf_sd_profile_event(PF_SD_PROFILE_TIMEOUT);
        goto data_done;
    }
    if (pf_storage_transport_exchange(&active_config.transport, NULL, data, size) != PF_STORAGE_TRANSPORT_OK ||
        !exchange_byte_checked(0xffu, &crc_high) ||
        !exchange_byte_checked(0xffu, &crc_low)) {
        goto data_done;
    }
    received = true;
data_done:
    pf_sd_profile_end(PF_SD_PROFILE_DATA, profile_start);
    if (!received) {
        return false;
    }

    uint16_t card_crc = ((uint16_t)crc_high << 8u) | crc_low;
    profile_start = pf_sd_profile_begin();
    uint16_t calculated_crc = pf_sd_crc16(data, size);
    pf_sd_profile_end(PF_SD_PROFILE_CRC, profile_start);
    if (card_crc != calculated_crc) {
        pf_sd_profile_event(PF_SD_PROFILE_CRC_FAILURE);
    }
    return card_crc == calculated_crc;
}

static bool receive_write_response(uint8_t *response)
{
    uint32_t start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);

    do {
        if (!exchange_byte_checked(0xffu, response)) {
            *response = 0xffu;
            return false;
        }
        if (!pf_sd_spi_write_response_is_pending(*response)) {
            return pf_sd_spi_write_response_is_accepted(*response);
        }
        pf_sd_idle();
    } while ((uint32_t)((uint32_t)pf_storage_transport_time_us(&active_config.transport) - start) < SD_SPI_RESPONSE_TIMEOUT_US);

    return false;
}

static bool read_csd(uint8_t csd[16])
{
    uint8_t response;
    bool success = false;

    select_card();
    if (wait_ready(SD_SPI_RESPONSE_TIMEOUT_US) &&
        send_command_selected(SD_CMD_SEND_CSD, 0u, &response) &&
        (response == 0u) && read_data_block(csd, 16u)) {
        success = true;
    }
    bool deselected = deselect_card();
    if (!deselected) {
        card_initialized = false;
    }
    return success && deselected;
}

static bool read_cid(uint8_t cid[16])
{
    uint8_t response;
    bool success = false;

    select_card();
    if (wait_ready(SD_SPI_RESPONSE_TIMEOUT_US) &&
        send_command_selected(SD_CMD_SEND_CID, 0u, &response) &&
        (response == 0u) && read_data_block(cid, 16u)) {
        success = true;
    }
    bool deselected = deselect_card();
    if (!deselected) {
        card_initialized = false;
    }
    return success && deselected;
}

static bool initialize_card(void)
{
    uint8_t response;
    uint8_t response_data[4];
    uint8_t csd[16];
    uint8_t cid[16];
    uint32_t response_argument;
    uint32_t ocr;
    uint32_t start;
    bool supports_v2;

    uint8_t idle_clocks[10];
    last_init_command = 0u;
    last_init_response = 0xffu;
    last_init_response_received = false;
    card_init_stage = PF_SD_SPI_INIT_STAGE_IDLE_CLOCKS;
    memset(idle_clocks, 0xff, sizeof(idle_clocks));
    if (pf_storage_transport_exchange(&active_config.transport, idle_clocks, NULL, sizeof(idle_clocks)) != PF_STORAGE_TRANSPORT_OK) {
        return false;
    }

    card_init_stage = PF_SD_SPI_INIT_STAGE_GO_IDLE;
    for (uint32_t attempt = 0u; attempt < SD_SPI_COMMAND_RETRIES; attempt++) {
        if (send_command(SD_CMD_GO_IDLE_STATE, 0u, &response, NULL, 0u) &&
            (response == SD_R1_IDLE_STATE)) {
            break;
        }
        if (attempt + 1u == SD_SPI_COMMAND_RETRIES) {
            return false;
        }
        pf_storage_transport_delay_ms(&active_config.transport, 10u);
    }

    card_init_stage = PF_SD_SPI_INIT_STAGE_IF_CONDITION;
    if (!send_command(SD_CMD_SEND_IF_COND, 0x000001aau, &response,
                      response_data, sizeof(response_data))) {
        return false;
    }
    response_argument = ((uint32_t)response_data[0] << 24u) |
                        ((uint32_t)response_data[1] << 16u) |
                        ((uint32_t)response_data[2] << 8u) |
                        (uint32_t)response_data[3];
    if ((response == (SD_R1_IDLE_STATE | SD_R1_ILLEGAL_COMMAND)) &&
        ((response_argument & 0xffu) == 0xffu)) {
        response_argument = 1u << 22u;
    } else if (response != SD_R1_IDLE_STATE) {
        return false;
    }
    if (!pf_sd_cmd8_supports_v2(response_argument, &supports_v2)) {
        return false;
    }

    card_init_stage = PF_SD_SPI_INIT_STAGE_OPERATING_CONDITION;
    start = (uint32_t)pf_storage_transport_time_us(&active_config.transport);
    do {
        if (!send_application_command(
                SD_ACMD_SD_SEND_OP_COND,
                0x00ff8000u | (supports_v2 ? SD_OCR_HCS : 0u), &response) ||
            !pf_sd_spi_r1_is_ready_or_idle(response)) {
            return false;
        }
        if (response == 0u) {
            break;
        }
        pf_storage_transport_delay_ms(&active_config.transport, 10u);
    } while ((uint32_t)((uint32_t)pf_storage_transport_time_us(&active_config.transport) - start) <
             SD_SPI_INITIALIZATION_TIMEOUT_US);
    if (response != 0u) {
        return false;
    }

    card_init_stage = PF_SD_SPI_INIT_STAGE_READ_OCR;
    if (!send_command(SD_CMD_READ_OCR, 0u, &response,
                      response_data, sizeof(response_data)) ||
        (response != 0u)) {
        return false;
    }
    ocr = ((uint32_t)response_data[0] << 24u) |
          ((uint32_t)response_data[1] << 16u) |
          ((uint32_t)response_data[2] << 8u) |
          (uint32_t)response_data[3];
    if (((ocr & SD_OCR_POWER_UP_STATUS) == 0u) ||
        ((ocr & SD_OCR_VOLTAGE_WINDOW) == 0u)) {
        return false;
    }
    card_block_addressed = supports_v2 && ((ocr & SD_OCR_CCS) != 0u);

    card_init_stage = PF_SD_SPI_INIT_STAGE_BLOCK_LENGTH;
    if (!card_block_addressed &&
        (!send_command(SD_CMD_SET_BLOCKLEN, PF_SD_BLOCK_SIZE,
                       &response, NULL, 0u) || (response != 0u))) {
        return false;
    }
    card_init_stage = PF_SD_SPI_INIT_STAGE_CSD;
    if (!read_csd(csd)) {
        return false;
    }
    card_init_stage = PF_SD_SPI_INIT_STAGE_CID;
    if (!read_cid(cid) ||
        !pf_sd_csd_sector_count(csd, &card_sector_count)) {
        return false;
    }

    uint32_t transfer_clock = (active_config.clock_hz == 0u)
        ? SD_SPI_DEFAULT_CLOCK_HZ
        : active_config.clock_hz;
    card_init_stage = PF_SD_SPI_INIT_STAGE_TRANSFER_CLOCK;
    if (pf_storage_transport_set_clock(&active_config.transport, transfer_clock, &card_bus_clock_hz) != PF_STORAGE_TRANSPORT_OK ||
        card_bus_clock_hz < SD_SPI_INIT_CLOCK_HZ) {
        return false;
    }
    memcpy(card_cid, cid, sizeof(card_cid));
    card_init_stage = PF_SD_SPI_INIT_STAGE_NONE;
    return true;
}

pf_sd_spi_init_stage_t pf_sd_spi_get_init_stage(void)
{
    return card_init_stage;
}

void pf_sd_spi_get_last_init_response(uint8_t *command, uint8_t *response,
                                      bool *received)
{
    if (command != NULL) {
        *command = last_init_command;
    }
    if (response != NULL) {
        *response = last_init_response;
    }
    if (received != NULL) {
        *received = last_init_response_received;
    }
}

static bool write_data_block(const uint8_t *buffer, uint8_t token)
{
    if (pf_storage_transport_exchange(&active_config.transport, &token, NULL, 1u) != PF_STORAGE_TRANSPORT_OK) {
        card_initialized = false;
        return false;
    }
    uint32_t profile_start = pf_sd_profile_begin();
    bool sent = pf_storage_transport_exchange(&active_config.transport, buffer, NULL, PF_SD_BLOCK_SIZE)
        == PF_STORAGE_TRANSPORT_OK;
    pf_sd_profile_end(PF_SD_PROFILE_DATA, profile_start);
    if (!sent) {
        card_initialized = false;
        return false;
    }
    profile_start = pf_sd_profile_begin();
    uint16_t crc = pf_sd_crc16(buffer, PF_SD_BLOCK_SIZE);
    pf_sd_profile_end(PF_SD_PROFILE_CRC, profile_start);
    uint8_t ignored;
    if (!exchange_byte_checked((uint8_t)(crc >> 8u), &ignored) ||
        !exchange_byte_checked((uint8_t)crc, &ignored)) {
        return false;
    }
    uint8_t response = 0xffu;
    bool accepted = receive_write_response(&response);
    /* Only CRC/write-error tokens conclusively reject this block. A missing or
     * malformed response may hide an accepted write; never replay it. Even a
     * rejection must reach ready before another command is safe. */
    bool rejected = ((response & 0x1fu) == 0x0bu) ||
                    ((response & 0x1fu) == 0x0du);
    bool ready = wait_ready(SD_SPI_WRITE_TIMEOUT_US);
    if ((!accepted && !rejected) || !ready) {
        card_initialized = false;
    }
    return accepted && ready;
}

static pf_sd_result_t transfer_block(uint32_t block, uint8_t *read_buffer,
                                          const uint8_t *write_buffer)
{
    uint8_t response;
    uint32_t argument;
    bool success = false;

    if (!card_is_present()) {
        card_initialized = false;
        return PF_SD_NOT_READY;
    }
    if (!pf_sd_spi_block_argument(block, card_block_addressed, &argument)) {
        return PF_SD_INVALID_ARGUMENT;
    }

    select_card();
    if (!wait_ready(SD_SPI_WRITE_TIMEOUT_US)) {
        card_initialized = false;
        goto transfer_done;
    }
    if (read_buffer != NULL) {
        if (!send_command_selected(SD_CMD_READ_SINGLE_BLOCK, argument, &response)) {
            /* The card may have accepted the command despite a lost R1. */
            card_initialized = false;
        } else if (response == 0u) {
            /* ponytail: any failed data phase (token timeout, short read, CRC)
             * invalidates; a CRC-only failure could be preserved but is not
             * distinguished. A non-zero R1 rejection keeps command mode known. */
            success = read_data_block(read_buffer, PF_SD_BLOCK_SIZE);
            if (!success) {
                card_initialized = false;
            }
        }
    } else {
        if (!send_command_selected(SD_CMD_WRITE_SINGLE_BLOCK, argument, &response)) {
            /* The card may have entered its data phase despite a lost R1. */
            card_initialized = false;
            goto transfer_done;
        }
        if (response != 0u) {
            goto transfer_done;
        }
        success = write_data_block(write_buffer, SD_DATA_START_BLOCK);
    }

transfer_done:
    if (!deselect_card()) {
        card_initialized = false;
        success = false;
    }
    return success ? PF_SD_OK : PF_SD_IO_ERROR;
}

pf_sd_result_t pf_sd_spi_init(const pf_sd_spi_config_t *config)
{
    if (!config_is_valid(config)) {
        return PF_SD_INVALID_ARGUMENT;
    }
    if (configuration_set && !same_config(&active_config, config)) {
        return PF_SD_CONFIG_LOCKED;
    }
    if (!configuration_set) {
        active_config = *config;
        if (pf_storage_transport_init(&active_config.transport) != PF_STORAGE_TRANSPORT_OK) {
            return PF_SD_INIT_FAILED;
        }
        spi_configured = true;
        configuration_set = true;
    }
    pf_sd_profile_set_clock(spi_profile_clock);

    card_initialized = false;
    if (spi_configured) {
        if (pf_storage_transport_set_clock(&active_config.transport, SD_SPI_INIT_CLOCK_HZ, &card_bus_clock_hz) != PF_STORAGE_TRANSPORT_OK) {
            return PF_SD_INIT_FAILED;
        }
    }
    if (!spi_configured || !card_is_present() || !initialize_card()) {
        card_initialized = false;
        return PF_SD_INIT_FAILED;
    }
    card_initialized = true;
    return PF_SD_OK;
}

bool pf_sd_spi_is_initialized(void)
{
    if (card_initialized && !card_is_present()) {
        card_initialized = false;
    }
    return card_initialized;
}

uint32_t pf_sd_spi_block_count(void)
{
    return pf_sd_spi_is_initialized() ? card_sector_count : 0u;
}

static pf_sd_result_t validate_transfer(uint32_t first_block,
                                             const void *buffer,
                                             uint32_t block_count)
{
    if (!pf_sd_spi_is_initialized()) {
        card_initialized = false;
        return PF_SD_NOT_READY;
    }
    return pf_sd_validate_transfer(first_block, buffer, block_count, card_sector_count);
}

static pf_sd_result_t transfer_blocks(uint32_t first_block, uint8_t *read_buffer,
                                           const uint8_t *write_buffer, uint32_t block_count)
{
    const void *buffer = read_buffer != NULL ? read_buffer : write_buffer;
    pf_sd_result_t result = validate_transfer(first_block, buffer, block_count);

    if (result != PF_SD_OK) {
        return result;
    }
    if (block_count > 1u) {
        uint32_t argument;
        uint8_t response = 0xffu;
        if (!pf_sd_block_argument(first_block, card_block_addressed, &argument)) {
            return PF_SD_INVALID_ARGUMENT;
        }
        select_card();
        bool command_ok = wait_ready(SD_SPI_WRITE_TIMEOUT_US) &&
            send_command_selected(read_buffer != NULL ? SD_CMD_READ_MULTIPLE_BLOCK
                                                      : SD_CMD_WRITE_MULTIPLE_BLOCK,
                                  argument, &response);
        if (!command_ok || response != 0u) {
            bool deselected = deselect_card();
            if (!deselected) {
                card_initialized = false;
                return PF_SD_IO_ERROR;
            }
            if (!command_ok) {
                card_initialized = false;
            }
            if (!command_ok || response != SD_R1_ILLEGAL_COMMAND) {
                return PF_SD_IO_ERROR;
            }
            pf_sd_profile_event(PF_SD_PROFILE_BATCH_FALLBACK);
        } else {
            bool success = true;
            for (uint32_t index = 0u; index < block_count; ++index) {
                if (!card_is_present()) {
                    card_initialized = false;
                    success = false;
                    break;
                }
                bool transferred = read_buffer != NULL
                    ? read_data_block(read_buffer + (size_t)index * PF_SD_BLOCK_SIZE,
                                      PF_SD_BLOCK_SIZE)
                    : write_data_block(write_buffer + (size_t)index * PF_SD_BLOCK_SIZE,
                                       SD_DATA_START_MULTIPLE);
                if (!transferred) {
                    success = false;
                    break;
                }
            }
            uint32_t profile_start = pf_sd_profile_begin();
            bool stopped;
            if (read_buffer != NULL) {
                stopped = send_command_selected(SD_CMD_STOP_TRANSMISSION, 0u, &response) &&
                    response == 0u && wait_ready(SD_SPI_WRITE_TIMEOUT_US);
            } else {
                stopped = wait_ready(SD_SPI_WRITE_TIMEOUT_US);
                if (stopped) {
                    uint8_t ignored;
                    stopped = exchange_byte_checked(SD_DATA_STOP_MULTIPLE, &ignored) &&
                        wait_ready(SD_SPI_WRITE_TIMEOUT_US);
                }
            }
            pf_sd_profile_end(PF_SD_PROFILE_STOP, profile_start);
            stopped = deselect_card() && stopped;
            if (!stopped) {
                card_initialized = false;
            }
            return success && stopped ? PF_SD_OK : PF_SD_IO_ERROR;
        }
    }
    for (uint32_t index = 0u; index < block_count; index++) {
        result = transfer_block(first_block + index,
                                read_buffer != NULL
                                    ? read_buffer + (size_t)index * PF_SD_BLOCK_SIZE : NULL,
                                write_buffer != NULL
                                    ? write_buffer + (size_t)index * PF_SD_BLOCK_SIZE : NULL);
        if (result != PF_SD_OK) {
            return result;
        }
    }
    return PF_SD_OK;
}

pf_sd_result_t pf_sd_spi_read_blocks(uint32_t first_block, void *buffer,
                                         uint32_t block_count)
{
    return transfer_blocks(first_block, buffer, NULL, block_count);
}

pf_sd_result_t pf_sd_spi_write_blocks(uint32_t first_block,
                                          const void *buffer,
                                          uint32_t block_count)
{
    return transfer_blocks(first_block, NULL, buffer, block_count);
}

pf_sd_result_t pf_sd_spi_sync(void)
{
    bool ready;

    if (!pf_sd_spi_is_initialized()) {
        card_initialized = false;
        return PF_SD_NOT_READY;
    }
    select_card();
    ready = wait_ready(SD_SPI_WRITE_TIMEOUT_US);
    ready = deselect_card() && ready;
    if (!ready) {
        card_initialized = false;
    }
    return ready ? PF_SD_OK : PF_SD_IO_ERROR;
}

void pf_sd_spi_get_info(pf_sd_info_t *info)
{
    if (info == NULL) {
        return;
    }
    *info = (pf_sd_info_t){ 0 };
    if (!configuration_set) {
        return;
    }
    if (pf_storage_transport_presence_known(&active_config.transport)) {
        info->presence_known = true;
        info->present = card_is_present();
    } else if (pf_sd_spi_is_initialized()) {
        info->presence_known = true;
        info->present = true;
    }
    info->initialized = pf_sd_spi_is_initialized();
    if (info->initialized) {
        info->cid_valid = true;
        info->bus_clock_hz = card_bus_clock_hz;
        info->block_count = card_sector_count;
        memcpy(info->cid, card_cid, sizeof(info->cid));
    }
}