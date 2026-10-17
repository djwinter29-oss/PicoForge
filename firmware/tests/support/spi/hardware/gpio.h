#pragma once
#include <stdbool.h>
#define NUM_BANK0_GPIOS 30
#define GPIO_OUT 1
#define GPIO_FUNC_SPI 1
extern bool spi_test_present;
static inline void gpio_init(unsigned pin) { (void)pin; }
static inline void gpio_set_dir(unsigned pin, unsigned direction) { (void)pin; (void)direction; }
static inline void gpio_set_function(unsigned pin, unsigned function) { (void)pin; (void)function; }
static inline void gpio_pull_up(unsigned pin) { (void)pin; }
static inline void gpio_pull_down(unsigned pin) { (void)pin; }
static inline void gpio_disable_pulls(unsigned pin) { (void)pin; }
static inline bool gpio_get(unsigned pin) { (void)pin; return spi_test_present; }
void gpio_put(unsigned pin, bool high);
