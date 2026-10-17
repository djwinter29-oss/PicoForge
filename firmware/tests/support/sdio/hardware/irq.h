#ifndef PF_TEST_SDIO_HARDWARE_IRQ_H
#define PF_TEST_SDIO_HARDWARE_IRQ_H

#include <stdbool.h>
#include "hardware/pio.h"

unsigned pio_get_irq_num(PIO pio, unsigned index);
void pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled);
void irq_add_shared_handler(unsigned irq, void (*handler)(void), unsigned order);
void irq_set_priority(unsigned irq, unsigned priority);
void irq_set_enabled(unsigned irq, bool enabled);

#endif
