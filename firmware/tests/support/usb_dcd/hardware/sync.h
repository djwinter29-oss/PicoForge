#ifndef PF_TEST_DCD_SYNC_H
#define PF_TEST_DCD_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/irq.h"

typedef struct { bool held; } spin_lock_t;
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t saved);
int spin_lock_claim_unused(bool required);
spin_lock_t *spin_lock_instance(uint number);
void spin_lock_unsafe_blocking(spin_lock_t *lock);
void spin_unlock_unsafe(spin_lock_t *lock);

#endif
