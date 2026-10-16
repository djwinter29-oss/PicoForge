#ifndef PF_TEST_SDIO_SDK_STUBS_H
#define PF_TEST_SDIO_SDK_STUBS_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"
#include "sdio_pio_host.h"

extern pf_sdio_pio_host_t *pf_test_sdio_observed_host;
extern bool pf_test_sdio_expect_streams_cleared;
extern bool pf_test_sdio_streams_were_cleared;
extern bool pf_test_sdio_can_add_program;
extern unsigned pf_test_sdio_removed_programs;

#endif
