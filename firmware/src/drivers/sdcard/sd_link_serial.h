/** @file
 * @brief Raw SPI operations implemented by the selected SPI engine; card framing belongs to sd_backend_spi.c.
 */
#ifndef PF_SD_LINK_SERIAL_H
#define PF_SD_LINK_SERIAL_H

#include "sd_link.h"

/** Assert/deassert CS for caller-sequenced raw byte exchanges. */
pf_sd_link_result_t pf_sd_link_serial_select(const pf_sd_link_t *transport, bool selected);
/** Exchange bytes without changing CS; NULL TX sends ones, NULL RX discards. */
pf_sd_link_result_t pf_sd_link_serial_exchange(const pf_sd_link_t *transport,
                                              const uint8_t *tx, uint8_t *rx, size_t length);

#endif
