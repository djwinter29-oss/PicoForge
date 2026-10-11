#include "sd_spi.h"

#include <stddef.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "sd_card_protocol.h"
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
#define SD_CMD_SET_BLOCKLEN 16u
#define SD_CMD_READ_SINGLE_BLOCK 17u
#define SD_CMD_WRITE_SINGLE_BLOCK 24u
#define SD_CMD_APP 55u
#define SD_CMD_READ_OCR 58u
#define SD_ACMD_SD_SEND_OP_COND 41u

#define SD_R1_IDLE_STATE 0x01u
#define SD_R1_ILLEGAL_COMMAND 0x04u
#define SD_DATA_START_BLOCK 0xfeu
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

static bool same_config(const pf_sd_spi_config_t *left,
                        const pf_sd_spi_config_t *right)
{
    return (left->spi == right->spi) &&
           (left->clock_gpio == right->clock_gpio) &&
           (left->mosi_gpio == right->mosi_gpio) &&
           (left->miso_gpio == right->miso_gpio) &&
           (left->chip_select_gpio == right->chip_select_gpio) &&
           (left->clock_hz == right->clock_hz) &&
           (left->use_card_detect == right->use_card_detect) &&
           (left->card_detect_gpio == right->card_detect_gpio) &&
           (left->card_detect_active_high == right->card_detect_active_high) &&
           (left->card_detect_use_pull == right->card_detect_use_pull) &&
           (left->card_detect_pull_up == right->card_detect_pull_up);
}

static bool pin_is_valid(uint32_t pin)
{
    return pin < NUM_BANK0_GPIOS;
}

static bool config_is_valid(const pf_sd_spi_config_t *config)
{
    uint32_t pins[4];

    if ((config == NULL) || ((config->spi != spi0) && (config->spi != spi1))) {
        return false;
    }

    pins[0] = config->clock_gpio;
    pins[1] = config->mosi_gpio;
    pins[2] = config->miso_gpio;
    pins[3] = config->chip_select_gpio;
    for (uint32_t pin_index = 0u; pin_index < 4u; pin_index++) {
        if (!pin_is_valid(pins[pin_index])) {
            return false;
        }
        for (uint32_t other_index = 0u; other_index < pin_index; other_index++) {
            if (pins[pin_index] == pins[other_index]) {
                return false;
            }
        }
    }

    if (!pf_sd_spi_pins_match_controller(spi_get_index(config->spi), config->clock_gpio,
                                         config->mosi_gpio, config->miso_gpio)) {
        return false;
    }

    if (config->use_card_detect &&
        (!pin_is_valid(config->card_detect_gpio) ||
         (config->card_detect_gpio == config->clock_gpio) ||
         (config->card_detect_gpio == config->mosi_gpio) ||
         (config->card_detect_gpio == config->miso_gpio) ||
         (config->card_detect_gpio == config->chip_select_gpio))) {
        return false;
    }

    return (config->clock_hz == 0u) ||
           ((config->clock_hz >= SD_SPI_INIT_CLOCK_HZ) &&
            (config->clock_hz <= SD_SPI_DEFAULT_CLOCK_HZ));
}

static bool card_is_present(void)
{
    return !active_config.use_card_detect ||
           ((gpio_get(active_config.card_detect_gpio) != 0u) ==
            active_config.card_detect_active_high);
}

static uint8_t exchange_byte(uint8_t value)
{
    uint8_t received = 0xffu;
    (void)spi_write_read_blocking(active_config.spi, &value, &received, 1u);
    return received;
}

static void select_card(void)
{
    gpio_put(active_config.chip_select_gpio, 0u);
}

static void deselect_card(void)
{
    gpio_put(active_config.chip_select_gpio, 1u);
    (void)exchange_byte(0xffu);
}

static bool wait_ready(uint32_t timeout_us)
{
    uint32_t start = time_us_32();

    do {
        if (exchange_byte(0xffu) == 0xffu) {
            return true;
        }
        pf_sd_card_idle();
    } while ((uint32_t)(time_us_32() - start) < timeout_us);

    return false;
}

static bool send_command_selected(uint8_t command, uint32_t argument,
                                  uint8_t *response)
{
    uint8_t frame[6];

    if ((response == NULL) ||
        !pf_sd_card_build_command(command, argument, frame) ||
        (spi_write_blocking(active_config.spi, frame, sizeof(frame)) !=
         (int)sizeof(frame))) {
        return false;
    }

    for (uint32_t attempt = 0u; attempt < SD_SPI_COMMAND_RETRIES; attempt++) {
        uint8_t value = exchange_byte(0xffu);
        if ((value & 0x80u) == 0u) {
            *response = value;
            return true;
        }
    }
    return false;
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
                   (spi_read_blocking(active_config.spi, 0xffu, extra,
                                      extra_size) == (int)extra_size));
    }
    deselect_card();
    return success;
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
    uint32_t start = time_us_32();
    uint8_t token;
    uint16_t card_crc;
    uint16_t calculated_crc;

    do {
        token = exchange_byte(0xffu);
        if (pf_sd_spi_read_token_is_valid(token)) {
            break;
        }
        if (token != 0xffu) {
            return false;
        }
        pf_sd_card_idle();
    } while ((uint32_t)(time_us_32() - start) < SD_SPI_RESPONSE_TIMEOUT_US);
    if (!pf_sd_spi_read_token_is_valid(token) ||
        (spi_read_blocking(active_config.spi, 0xffu, data, size) != (int)size)) {
        return false;
    }

    card_crc = (uint16_t)exchange_byte(0xffu) << 8u;
    card_crc |= exchange_byte(0xffu);
    calculated_crc = pf_sd_card_crc16(data, size);
    return card_crc == calculated_crc;
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
    deselect_card();
    return success;
}

static bool initialize_card(void)
{
    uint8_t response;
    uint8_t response_data[4];
    uint8_t csd[16];
    uint32_t response_argument;
    uint32_t ocr;
    uint32_t start;
    bool supports_v2;

    uint8_t idle_clocks[10];
    memset(idle_clocks, 0xff, sizeof(idle_clocks));
    if (spi_write_blocking(active_config.spi, idle_clocks,
                           sizeof(idle_clocks)) != (int)sizeof(idle_clocks)) {
        return false;
    }

    for (uint32_t attempt = 0u; attempt < SD_SPI_COMMAND_RETRIES; attempt++) {
        if (send_command(SD_CMD_GO_IDLE_STATE, 0u, &response, NULL, 0u) &&
            (response == SD_R1_IDLE_STATE)) {
            break;
        }
        if (attempt + 1u == SD_SPI_COMMAND_RETRIES) {
            return false;
        }
        sleep_ms(10u);
    }

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
    if (!pf_sd_card_cmd8_supports_v2(response_argument, &supports_v2)) {
        return false;
    }

    start = time_us_32();
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
        sleep_ms(10u);
    } while ((uint32_t)(time_us_32() - start) <
             SD_SPI_INITIALIZATION_TIMEOUT_US);
    if (response != 0u) {
        return false;
    }

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

    if (!card_block_addressed &&
        (!send_command(SD_CMD_SET_BLOCKLEN, PF_SD_CARD_BLOCK_SIZE,
                       &response, NULL, 0u) || (response != 0u))) {
        return false;
    }
    if (!read_csd(csd) ||
        !pf_sd_card_csd_sector_count(csd, &card_sector_count)) {
        return false;
    }

    uint32_t transfer_clock = (active_config.clock_hz == 0u)
        ? SD_SPI_DEFAULT_CLOCK_HZ
        : active_config.clock_hz;
    return spi_set_baudrate(active_config.spi, transfer_clock) >=
           SD_SPI_INIT_CLOCK_HZ;
}

static pf_sd_card_result_t transfer_block(uint32_t block, uint8_t *read_buffer,
                                          const uint8_t *write_buffer)
{
    uint8_t response;
    uint32_t argument;
    bool success = false;

    if (!card_is_present()) {
        card_initialized = false;
        return PF_SD_CARD_NOT_READY;
    }
    if (!pf_sd_spi_block_argument(block, card_block_addressed, &argument)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }

    select_card();
    if (!wait_ready(SD_SPI_WRITE_TIMEOUT_US)) {
        goto transfer_done;
    }
    if (read_buffer != NULL) {
        if (send_command_selected(SD_CMD_READ_SINGLE_BLOCK, argument, &response) &&
            (response == 0u) && read_data_block(read_buffer, PF_SD_CARD_BLOCK_SIZE)) {
            success = true;
        }
    } else {
        uint16_t crc;
        uint8_t data_response;
        uint8_t token = SD_DATA_START_BLOCK;

        if (!send_command_selected(SD_CMD_WRITE_SINGLE_BLOCK, argument, &response) ||
            (response != 0u)) {
            goto transfer_done;
        }
        if ((spi_write_blocking(active_config.spi, &token, 1u) != 1) ||
            (spi_write_blocking(active_config.spi, write_buffer,
                                PF_SD_CARD_BLOCK_SIZE) !=
             (int)PF_SD_CARD_BLOCK_SIZE)) {
            goto transfer_done;
        }
        crc = pf_sd_card_crc16(write_buffer, PF_SD_CARD_BLOCK_SIZE);
        (void)exchange_byte((uint8_t)(crc >> 8u));
        (void)exchange_byte((uint8_t)crc);
        data_response = exchange_byte(0xffu);
        success = pf_sd_spi_write_response_is_accepted(data_response) &&
                  wait_ready(SD_SPI_WRITE_TIMEOUT_US);
    }

transfer_done:
    deselect_card();
    return success ? PF_SD_CARD_OK : PF_SD_CARD_IO_ERROR;
}

pf_sd_card_result_t pf_sd_spi_init(const pf_sd_spi_config_t *config)
{
    if (!config_is_valid(config)) {
        return PF_SD_CARD_INVALID_ARGUMENT;
    }
    if (configuration_set && !same_config(&active_config, config)) {
        return PF_SD_CARD_CONFIG_LOCKED;
    }
    if (!configuration_set) {
        active_config = *config;

        gpio_init(active_config.chip_select_gpio);
        gpio_put(active_config.chip_select_gpio, 1u);
        gpio_set_dir(active_config.chip_select_gpio, GPIO_OUT);
        if (active_config.use_card_detect) {
            gpio_init(active_config.card_detect_gpio);
            if (active_config.card_detect_use_pull) {
                if (active_config.card_detect_pull_up) {
                    gpio_pull_up(active_config.card_detect_gpio);
                } else {
                    gpio_pull_down(active_config.card_detect_gpio);
                }
            }
        }

        if (spi_init(active_config.spi, SD_SPI_INIT_CLOCK_HZ) == 0u) {
            return PF_SD_CARD_INIT_FAILED;
        }
        spi_set_format(active_config.spi, 8u, SPI_CPOL_0, SPI_CPHA_0,
                       SPI_MSB_FIRST);
        gpio_set_function(active_config.clock_gpio, GPIO_FUNC_SPI);
        gpio_set_function(active_config.mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(active_config.miso_gpio, GPIO_FUNC_SPI);
        spi_configured = true;
        configuration_set = true;
    }

    card_initialized = false;
    if (spi_configured) {
        (void)spi_set_baudrate(active_config.spi, SD_SPI_INIT_CLOCK_HZ);
    }
    if (!spi_configured || !card_is_present() || !initialize_card()) {
        card_initialized = false;
        return PF_SD_CARD_INIT_FAILED;
    }
    card_initialized = true;
    return PF_SD_CARD_OK;
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

static pf_sd_card_result_t validate_transfer(uint32_t first_block,
                                             const void *buffer,
                                             uint32_t block_count)
{
    if (!pf_sd_spi_is_initialized()) {
        card_initialized = false;
        return PF_SD_CARD_NOT_READY;
    }
    return pf_sd_card_validate_transfer(first_block, buffer, block_count, card_sector_count);
}

pf_sd_card_result_t pf_sd_spi_read_blocks(uint32_t first_block, void *buffer,
                                         uint32_t block_count)
{
    pf_sd_card_result_t result = validate_transfer(first_block, buffer, block_count);

    if (result != PF_SD_CARD_OK) {
        return result;
    }
    for (uint32_t index = 0u; index < block_count; index++) {
        result = transfer_block(first_block + index,
                                (uint8_t *)buffer +
                                    (size_t)index * PF_SD_CARD_BLOCK_SIZE,
                                NULL);
        if (result != PF_SD_CARD_OK) {
            return result;
        }
    }
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sd_spi_write_blocks(uint32_t first_block,
                                          const void *buffer,
                                          uint32_t block_count)
{
    pf_sd_card_result_t result = validate_transfer(first_block, buffer, block_count);

    if (result != PF_SD_CARD_OK) {
        return result;
    }
    for (uint32_t index = 0u; index < block_count; index++) {
        result = transfer_block(first_block + index, NULL,
                                (const uint8_t *)buffer +
                                    (size_t)index * PF_SD_CARD_BLOCK_SIZE);
        if (result != PF_SD_CARD_OK) {
            return result;
        }
    }
    return PF_SD_CARD_OK;
}

pf_sd_card_result_t pf_sd_spi_sync(void)
{
    bool ready;

    if (!pf_sd_spi_is_initialized()) {
        card_initialized = false;
        return PF_SD_CARD_NOT_READY;
    }
    select_card();
    ready = wait_ready(SD_SPI_WRITE_TIMEOUT_US);
    deselect_card();
    return ready ? PF_SD_CARD_OK : PF_SD_CARD_IO_ERROR;
}