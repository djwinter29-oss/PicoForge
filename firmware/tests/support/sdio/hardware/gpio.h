#ifndef PF_TEST_SDIO_HARDWARE_GPIO_H
#define PF_TEST_SDIO_HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

uint32_t gpio_get(unsigned pin);
void gpio_pull_up(unsigned pin);

#endif
