#include <stdio.h>
#include <string.h>

#include "board/board_nor.h"
#include "pico/stdlib.h"

int main(void)
{
    stdio_init_all();
    pf_nor_transport_t transport;
    pf_nor_flash_t flash;
    uint8_t single[32], quad[32];
    pf_nor_result_t result = pf_board_nor_init(&transport);
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
    pf_board_nor_deinit();
    for (;;) {
        sleep_ms(1000u);
    }
}
