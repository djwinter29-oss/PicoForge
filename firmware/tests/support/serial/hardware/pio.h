#pragma once
#include "../serial_fake.h"
#define NUM_BANK0_GPIOS 30u
typedef struct { unsigned index; struct { uint32_t clkdiv; } sm[4]; } serial_pio_t;
typedef serial_pio_t *PIO;
extern serial_pio_t serial_pio[2];
#define pio0 (&serial_pio[0])
#define pio1 (&serial_pio[1])
typedef struct { uint lanes; } pio_sm_config;
typedef struct { uint length; } pio_program_t;
static inline int pio_claim_unused_sm(PIO pio, bool required) { (void)pio; (void)required; return 0; }
static inline void pio_sm_unclaim(PIO pio, uint sm) { (void)pio; (void)sm; serial_fake.unclaim++; }
static inline bool pio_can_add_program(PIO pio, const pio_program_t *program)
{ (void)pio; (void)program; return !serial_fake.no_program; }
static inline uint pio_add_program(PIO pio, const pio_program_t *program)
{ (void)pio; (void)program; return serial_fake.program_count++ * 4; }
static inline void pio_remove_program(PIO pio, const pio_program_t *program, uint offset)
{ (void)pio; (void)program; (void)offset; serial_fake.removes++; }
static inline void pio_gpio_init(PIO pio, uint pin) { (void)pio; (void)pin; }
static inline void pio_sm_set_enabled(PIO pio, uint sm, bool enabled)
{ (void)pio; (void)sm; (void)enabled; }
static inline void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config)
{ (void)pio; (void)sm; (void)offset; serial_fake.lanes = config->lanes; serial_fake.configure++; }
static inline void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint values, uint mask)
{ (void)pio; (void)sm; (void)values; (void)mask; }
static inline void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint values, uint mask)
{ (void)pio; (void)sm; (void)mask; serial_fake.output_mask = values; }
static inline void pio_sm_clear_fifos(PIO pio, uint sm) { (void)pio; (void)sm; }
static inline bool pio_sm_is_tx_fifo_full(PIO pio, uint sm)
{ (void)pio; (void)sm; return serial_fake.fifo_full; }
static inline bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) { (void)pio; (void)sm; return false; }
static inline void pio_sm_put(PIO pio, uint sm, uint32_t value)
{
    (void)pio; (void)sm;
    serial_fake.symbols++;
    if (serial_fake.length < sizeof(serial_fake.bytes))
        serial_fake.bytes[serial_fake.length++] = (uint8_t)(value >> (32u - serial_fake.lanes));
}
static inline uint32_t pio_sm_get(PIO pio, uint sm) { (void)pio; (void)sm; return serial_fake.input; }
static inline void sm_config_set_out_pins(pio_sm_config *config, uint pin, uint count)
{ (void)pin; config->lanes = count; }
static inline void sm_config_set_in_pins(pio_sm_config *config, uint pin) { (void)config; (void)pin; }
static inline void sm_config_set_sideset_pins(pio_sm_config *config, uint pin) { (void)config; (void)pin; }
static inline void sm_config_set_out_shift(pio_sm_config *config, bool right, bool automatic, uint threshold)
{ (void)config; (void)right; (void)automatic; (void)threshold; }
static inline void sm_config_set_in_shift(pio_sm_config *config, bool right, bool automatic, uint threshold)
{ (void)config; (void)right; (void)automatic; (void)threshold; }
static inline void sm_config_set_clkdiv(pio_sm_config *config, float divider) { (void)config; (void)divider; }
