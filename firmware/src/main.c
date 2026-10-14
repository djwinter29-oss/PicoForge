#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "board/board.h"
#include "drivers/sd_card/sdio_pio/sdio_pio.h"
#include "drivers/led.h"
#include "drivers/temperature.h"
#include "middleware/logging/logging.h"
#include "integration/freertos/sd_card_wait.h"
#include "usb/usb_task.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    panic("FreeRTOS stack overflow in task %s", task_name);
}

int main(void)
{
    if (!pf_board_init()) {
        panic("Failed to initialize board pin assignments");
    }
    pf_log_init();
    PF_LOG_INFO("boot", "Firmware initialization started");

    pf_temperature_init();

    pf_sd_card_config_t sd_card_config;
    if (pf_board_get_sd_card_config(&sd_card_config)) {
        pf_sd_card_wait_init(&sd_card_config);
        pf_sd_card_result_t sd_card_result = pf_sd_card_init(&sd_card_config);
        if (sd_card_result == PF_SD_CARD_OK) {
            PF_LOG_INFO("storage", "SD card initialized");
        } else {
            pf_sd_card_info_t sd_card_info;
            const char *presence = "unknown";

            pf_sd_card_get_info(&sd_card_info);
            if (sd_card_info.presence_known) {
                presence = sd_card_info.present ? "present" : "absent";
            }
            PF_LOG_WARNING("storage", "SD init failed result=%u detect=%s",
                           (unsigned)sd_card_result, presence);
            const pf_board_resources_t *topology = pf_board_resources_get();
            if (topology->card_detect_enabled) {
                uint32_t detect_gpio = topology->card_detect_gpio;
                PF_LOG_WARNING("storage",
                               "DET GP%u level=%u direction=%s function=%u pull_up=%u pull_down=%u",
                               (unsigned)detect_gpio, (unsigned)gpio_get(detect_gpio),
                               gpio_get_dir(detect_gpio) ? "OUT" : "IN",
                               (unsigned)gpio_get_function(detect_gpio),
                               (unsigned)gpio_is_pulled_up(detect_gpio),
                               (unsigned)gpio_is_pulled_down(detect_gpio));
            }
            if (sd_card_config.transport == PF_SD_CARD_TRANSPORT_SDIO_PIO) {
                uint8_t response[6];
                uint8_t last_command;
                uint32_t last_argument;
                uint32_t last_response_argument;

                pf_sdio_pio_get_init_response(response);
                pf_sdio_pio_get_last_short_response(&last_command, &last_argument,
                                                    &last_response_argument);
                PF_LOG_WARNING("storage", "SDIO init stage=%u",
                               (unsigned)pf_sdio_pio_get_init_stage());
                PF_LOG_WARNING("storage", "last CMD%u arg=%08lx R1=%08lx",
                               (unsigned)last_command, (unsigned long)last_argument,
                               (unsigned long)last_response_argument);
                if ((pf_sdio_pio_get_init_stage() == PF_SDIO_PIO_INIT_STAGE_BUS_WIDTH_PREFIX) ||
                    (pf_sdio_pio_get_init_stage() == PF_SDIO_PIO_INIT_STAGE_OPERATING_RESPONSE)) {
                    uint32_t words[5];
                    size_t word_count;

                    pf_sdio_pio_get_last_response_words(words, &word_count);
                    PF_LOG_WARNING("storage", "SDIO RX words=%u %08lx %08lx",
                                   (unsigned)word_count, (unsigned long)words[0],
                                   (unsigned long)words[1]);
                }
                PF_LOG_WARNING("storage", "SDIO response=%02x%02x%02x%02x%02x%02x",
                               (unsigned)response[0], (unsigned)response[1],
                               (unsigned)response[2], (unsigned)response[3],
                               (unsigned)response[4], (unsigned)response[5]);
            } else if (sd_card_config.transport == PF_SD_CARD_TRANSPORT_SPI) {
                uint8_t command;
                uint8_t response;
                bool received;

                pf_sd_spi_get_last_init_response(&command, &response, &received);
                PF_LOG_WARNING("storage", "SPI init stage=%u",
                               (unsigned)pf_sd_spi_get_init_stage());
                PF_LOG_WARNING("storage", "SPI last CMD%u response=%02x received=%u",
                               (unsigned)command, (unsigned)response, (unsigned)received);
            }
        }
    } else {
        PF_LOG_WARNING("storage", "No valid SD-card board configuration");
    }

    pf_usb_start();
    PF_LOG_INFO("usb", "CDC and HID interfaces initialized");

    vTaskStartScheduler();
    panic("FreeRTOS scheduler returned");
}