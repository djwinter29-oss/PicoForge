#ifndef PF_SERIAL_FAKE_H
#define PF_SERIAL_FAKE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned int uint;
typedef struct {
    unsigned init, deinit, baud, cs_low, cs_high, unclaim, removes, program_count, symbols, configure;
    uint32_t hz, now, output_mask, lanes;
    uint32_t gpio_initialized, output_pins, high_pins;
    uint8_t bytes[128], input;
    size_t length;
    bool short_io, fifo_full, selected, no_program;
    unsigned pending, peak_pending, delayed_reads;
    bool fifo_write;
    uint32_t shift_bits; /**< PIO automatic pull/push threshold. */
    uint32_t system_hz; /**< Optional simulated system clock; zero selects the SDK default. */
} serial_fake_t;
extern serial_fake_t serial_fake;
#endif
