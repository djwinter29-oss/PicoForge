#ifndef PF_TEST_SDIO_HARDWARE_PIO_H
#define PF_TEST_SDIO_HARDWARE_PIO_H
#define PF_TEST_SDIO_FAKE_PIO 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUM_BANK0_GPIOS 30u
typedef unsigned int uint;
typedef struct {
    uint32_t txf[4];
    uint32_t rxf[4];
    uint32_t input_sync_bypass;
    uint32_t fdebug;
} pf_test_sdio_pio_t;
typedef pf_test_sdio_pio_t *PIO;
extern PIO pio0;
extern PIO pio1;

typedef struct {
    const uint16_t *instructions;
    size_t length;
} pio_program_t;

typedef struct {
    uint32_t opaque[8];
} pio_sm_config;

typedef enum {
    pis_interrupt0,
    pis_interrupt1,
    pis_interrupt2,
    pis_interrupt3
} pio_interrupt_source_t;

enum {
    pio_y = 2u,
    pio_osr = 3u,
    PIO_ORIGIN_RX = 0u,
    PIO_ORIGIN_TX = 1u,
    STATUS_TX_LESSTHAN = 0u
};

bool pio_can_add_program(PIO pio, const pio_program_t *program);
uint pio_add_program(PIO pio, const pio_program_t *program);
void pio_remove_program(PIO pio, const pio_program_t *program, uint offset);
uint pio_claim_unused_sm(PIO pio, bool required);
void pio_sm_unclaim(PIO pio, uint sm);
void pio_gpio_init(PIO pio, uint pin);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out);
void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_enable_sm_mask_in_sync(PIO pio, uint mask);
uint pio_get_dreq(PIO pio, uint sm, bool tx);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t value);
void pio_sm_exec(PIO pio, uint sm, uint instruction);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_restart(PIO pio, uint sm);
void pio_clkdiv_restart_sm_mask(PIO pio, uint32_t mask);
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm);
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm);
uint32_t pio_sm_get(PIO pio, uint sm);
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t integer, uint8_t fraction);
void pio_interrupt_clear(PIO pio, uint index);
bool pio_interrupt_get(PIO pio, uint index);
uint pio_encode_jmp(uint address);
uint pio_encode_pull(bool if_empty, bool block);
uint pio_encode_mov(uint destination, uint source);
uint pio_encode_set(uint destination, uint value);
pio_sm_config pio_get_default_sm_config(void);
uint pio_encode_in(uint source, uint count);
uint pio_encode_out(uint destination, uint count);
uint pio_encode_wait_gpio(bool polarity, uint pin);
uint pio_encode_wait_pin(bool polarity, uint pin);
uint pio_encode_wait_irq(bool polarity, bool relative, uint irq);
uint pio_encode_irq_set(bool relative, uint irq);
uint pio_encode_irq_wait(bool relative, uint irq);
uint pio_encode_nop(void);
uint pio_encode_sideset(uint sideset);
void sm_config_set_in_pins(pio_sm_config *config, uint pin);
void sm_config_set_out_pins(pio_sm_config *config, uint pin, uint count);
void sm_config_set_set_pins(pio_sm_config *config, uint pin, uint count);
void sm_config_set_sideset_pins(pio_sm_config *config, uint pin);
void sm_config_set_in_shift(pio_sm_config *config, bool right, bool autopush, uint threshold);
void sm_config_set_out_shift(pio_sm_config *config, bool right, bool autopull, uint threshold);
void sm_config_set_fifo_join(pio_sm_config *config, uint join);
void sm_config_set_clkdiv_int_frac(pio_sm_config *config, uint16_t integer, uint8_t fraction);
void sm_config_set_sideset(pio_sm_config *config, uint bit_count, bool optional, bool pindirs);
void sm_config_set_jmp_pin(pio_sm_config *config, uint pin);
void sm_config_set_mov_status(pio_sm_config *config, uint selector, uint threshold);

#endif
