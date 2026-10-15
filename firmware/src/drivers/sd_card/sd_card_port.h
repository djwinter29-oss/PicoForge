#ifndef PF_SD_CARD_PORT_H
#define PF_SD_CARD_PORT_H

#include "sd_card.h"

/** Initialize the selected execution-environment port before card use.
 * Call once before card access or card-using worker startup. FreeRTOS may run
 * this before scheduler startup to install optional PIO waits.
 * Bare metal: no setup required. Configuration is borrowed only for this call.
 * This does not initialize the card or access media.
 */
void pf_sd_card_port_init(const pf_sd_card_config_t *config);

/** Internal recursive reservation primitives used by the shared facade.
 * FreeRTOS requires task context with the scheduler running.
 * Bare metal requires one foreground context: no ISR, reentrant callback, or
 * second-core callers. Its reservations are no-ops and try-lock always succeeds.
 * Neither port disables interrupts for the duration of a transfer.
 */
void pf_sd_card_port_lock(void);
bool pf_sd_card_port_try_lock(void);
void pf_sd_card_port_unlock(void);

#endif
