#include <stdio.h>
#include <string.h>

#include "board/board_storage.h"
#include "drivers/nor_flash/nor_flash.h"
#include "pico/stdlib.h"

int main(void)
{
    stdio_init_all();
    pf_nor_transport_t transport;
    pf_storage_bus_t bus = {0};
    pf_nor_flash_t flash;
    uint8_t single[32], quad[32];
    pf_nor_result_t result = pf_nor_result_from_storage(pf_board_storage_prepare(&bus));
    if (result == PF_NOR_OK) {
        result = pf_nor_transport_init(&bus, &transport);
    }
    if (result == PF_NOR_OK) {
        result = pf_nor_flash_init(&flash, &transport);
    }
    if (result == PF_NOR_OK) {
        result = pf_nor_flash_read(&flash, 0u, single, sizeof(single));
    }
#if PF_NOR_QSPI
    if (result == PF_NOR_OK) {
        result = pf_nor_flash_enable_quad(&flash);
    }
    if (result == PF_NOR_OK) {
        result = pf_nor_flash_read(&flash, 0u, quad, sizeof(quad));
    }
    if (result == PF_NOR_OK && memcmp(single, quad, sizeof(single)) != 0) {
        result = PF_NOR_VERIFY_ERROR;
    }
#else
    (void)quad;
#endif
    printf("Raw NOR identify/read smoke: result=%d (no program/erase performed)\n", (int)result);
    pf_storage_bus_deinit(&bus);
    pf_board_storage_release_pins();
    for (;;) {
        sleep_ms(1000u);
    }
}
