#ifndef PF_TEST_HARDWARE_GPIO_H
#define PF_TEST_HARDWARE_GPIO_H

#include <stdbool.h>

#define NUM_BANK0_GPIOS 30u
#define GPIO_OUT 1

typedef unsigned int uint;

void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_put(uint gpio, bool value);

#endif
