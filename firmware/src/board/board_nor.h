#ifndef PF_BOARD_NOR_H
#define PF_BOARD_NOR_H

#include "drivers/nor_flash/nor_flash.h"

/** Initialize the dedicated GP2..GP7 NOR transport selected at build time.
 * Mutually exclusive with SD on the reference wiring. Does not touch flash contents.
 * The board owns the transport context; serialize calls and deinit before reinit.
 */
pf_nor_result_t pf_board_nor_init(pf_nor_transport_t *transport);
/** Release board-owned NOR transport resources after all operations finish. */
void pf_board_nor_deinit(void);

#endif
