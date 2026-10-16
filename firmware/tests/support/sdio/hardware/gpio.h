#ifndef PF_TEST_SDIO_HARDWARE_GPIO_H
#define PF_TEST_SDIO_HARDWARE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

uint32_t gpio_get(unsigned pin);
void gpio_init(unsigned pin);
void gpio_put(unsigned pin, bool value);
void gpio_set_dir(unsigned pin, bool out);
void gpio_pull_up(unsigned pin);
void gpio_pull_down(unsigned pin);
void gpio_disable_pulls(unsigned pin);

#endif
