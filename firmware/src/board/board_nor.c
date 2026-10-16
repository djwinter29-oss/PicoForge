#include "board_nor.h"

#if PF_NOR_QSPI
#include "drivers/nor_flash/qspi_pio/nor_qspi_pio.h"
static pf_nor_qspi_pio_t bus = {
    .pio = pio0, .clock_gpio = 2u, .data_gpio = 3u, .cs_gpio = 7u,
    .clock_hz = 1000000u,
};
#else
#include "drivers/nor_flash/spi/nor_spi.h"
static pf_nor_spi_t bus = {
    .instance = spi0, .clock_gpio = 2u, .mosi_gpio = 3u, .miso_gpio = 4u,
    .wp_gpio = 5u, .hold_gpio = 6u, .cs_gpio = 7u, .clock_hz = 1000000u,
};
#endif

pf_nor_result_t pf_board_nor_init(pf_nor_transport_t *transport)
{
#if PF_NOR_QSPI
    return pf_nor_qspi_pio_init(&bus, transport);
#else
    return pf_nor_spi_init(&bus, transport);
#endif
}

void pf_board_nor_deinit(void)
{
#if PF_NOR_QSPI
    pf_nor_qspi_pio_deinit(&bus);
#else
    pf_nor_spi_deinit(&bus);
#endif
}
