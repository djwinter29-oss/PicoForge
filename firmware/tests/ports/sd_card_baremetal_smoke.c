#include "sd_card.h"
#include "sd_card_port.h"

#include <stddef.h>

/* Link every facade data path without initializing hardware or touching media. */
int main(void)
{
    pf_sd_card_info_t info;
    pf_sd_card_port_init(NULL);
    pf_sd_card_lock();
    if (!pf_sd_card_try_lock()) {
        return 1;
    }
    pf_sd_card_unlock();
    pf_sd_card_unlock();
    if (pf_sd_card_init(NULL) != PF_SD_CARD_INVALID_ARGUMENT) {
        return 2;
    }
    pf_sd_card_get_info(&info);
    if (info.initialized || pf_sd_card_is_initialized() || pf_sd_card_block_count() != 0u) {
        return 3;
    }
    if (pf_sd_card_read_blocks(0u, NULL, 0u) != PF_SD_CARD_NOT_CONFIGURED ||
        pf_sd_card_write_blocks(0u, NULL, 0u) != PF_SD_CARD_NOT_CONFIGURED ||
        pf_sd_card_sync() != PF_SD_CARD_NOT_CONFIGURED) {
        return 4;
    }
    return 0;
}
