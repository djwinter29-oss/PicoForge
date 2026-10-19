#ifndef HARDWARE_FLASH_H
#define HARDWARE_FLASH_H

#include <stddef.h>
#include <stdint.h>

#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#define PICO_OK 0
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u

/* Host model of memory-mapped flash; XIP_BASE is the array's address. */
extern uint8_t pf_test_flash[PICO_FLASH_SIZE_BYTES];
extern unsigned pf_test_erase_calls;
#define XIP_BASE ((uintptr_t)pf_test_flash)

void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);

#endif
