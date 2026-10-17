#ifndef STORAGE_TEST_STDLIB_H
#define STORAGE_TEST_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
void sleep_ms(uint32_t ms);
void panic(const char *format, ...);
#endif
