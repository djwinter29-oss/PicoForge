#include "sd.h"
#include "sd_port.h"

#include <stddef.h>

/* Link every facade data path without initializing hardware or touching media. */
int main(void)
{
    pf_sd_info_t info;
    if (!pf_sd_port_init(NULL)) {
        return 1;
    }
    pf_sd_lock();
    if (!pf_sd_try_lock()) {
        return 1;
    }
    pf_sd_unlock();
    pf_sd_unlock();
    if (pf_sd_init(NULL) != PF_SD_INVALID_ARGUMENT) {
        return 2;
    }
    pf_sd_get_info(&info);
    if (info.initialized || pf_sd_is_initialized() || pf_sd_block_count() != 0u) {
        return 3;
    }
    if (pf_sd_read_blocks(0u, NULL, 0u) != PF_SD_NOT_CONFIGURED ||
        pf_sd_write_blocks(0u, NULL, 0u) != PF_SD_NOT_CONFIGURED ||
        pf_sd_sync() != PF_SD_NOT_CONFIGURED) {
        return 4;
    }
    return 0;
}
