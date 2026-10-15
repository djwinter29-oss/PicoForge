#pragma once
#include <stdbool.h>
#define NUM_BANK0_GPIOS 30
#define GPIO_OUT 1
#define GPIO_FUNC_SPI 1
static inline void gpio_init(unsigned p) { (void)p; }
static inline void gpio_set_dir(unsigned p, unsigned d) { (void)p;(void)d; }
static inline void gpio_set_function(unsigned p, unsigned f) { (void)p;(void)f; }
static inline void gpio_pull_up(unsigned p) { (void)p; }
static inline void gpio_pull_down(unsigned p) { (void)p; }
static inline void gpio_disable_pulls(unsigned p) { (void)p; }
static inline bool gpio_get(unsigned p) { (void)p; return true; }
void gpio_put(unsigned, bool);
