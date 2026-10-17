#pragma once
#include "hardware/pio.h"
static const pio_program_t pf_storage_single_program = {4};
static const pio_program_t pf_storage_quad_program = {4};
static inline pio_sm_config pf_storage_single_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){1}; }
static inline pio_sm_config pf_storage_quad_program_get_default_config(uint offset)
{ (void)offset; return (pio_sm_config){4}; }
