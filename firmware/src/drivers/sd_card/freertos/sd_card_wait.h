#ifndef PF_SD_CARD_WAIT_H
#define PF_SD_CARD_WAIT_H

#include "sd_card.h"

/** Install the optional native completion adapter before scheduler/card task use.
 * Reserves task-notification index 1 and the selected PIO's IRQ0 completion
 * sources when PF_CONFIG_SD_CARD_INTERRUPT_WAIT is enabled. No-op otherwise.
 */
void pf_sd_card_wait_init(const pf_sd_card_config_t *config);

#endif