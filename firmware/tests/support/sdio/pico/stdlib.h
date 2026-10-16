#ifndef PF_TEST_SDIO_PICO_STDLIB_H
#define PF_TEST_SDIO_PICO_STDLIB_H

#include <stdint.h>

uint32_t time_us_32(void);
void sleep_us(uint64_t duration);
void sleep_ms(uint32_t duration);
void tight_loop_contents(void);

#endif
