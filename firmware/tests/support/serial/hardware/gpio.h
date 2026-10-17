#pragma once
#include "../serial_fake.h"
#define NUM_BANK0_GPIOS 30u
#define GPIO_OUT 1u
#define GPIO_FUNC_SPI 1u
static inline void gpio_init(uint pin)
{
    serial_fake.gpio_initialized |= 1u << pin;
    serial_fake.output_pins &= ~(1u << pin);
}
static inline void gpio_set_dir(uint pin, bool output)
{
    if (output) serial_fake.output_pins |= 1u << pin;
    else serial_fake.output_pins &= ~(1u << pin);
}
static inline void gpio_set_function(uint pin, uint function) { (void)pin; (void)function; }
static inline void gpio_pull_up(uint pin) { (void)pin; }
static inline void gpio_put(uint pin, bool high)
{
    if (high) serial_fake.high_pins |= 1u << pin;
    else serial_fake.high_pins &= ~(1u << pin);
    if (high) serial_fake.cs_high++; else serial_fake.cs_low++;
    serial_fake.selected = !high;
}
