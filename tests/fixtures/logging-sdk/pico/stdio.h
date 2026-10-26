#ifndef PF_TEST_PICO_STDIO_H
#define PF_TEST_PICO_STDIO_H

#include <stdbool.h>

#ifndef PICO_STDOUT_MUTEX
#define PICO_STDOUT_MUTEX 1
#endif

typedef struct stdio_driver {
    int unused;
} stdio_driver_t;

extern stdio_driver_t stdio_uart;

void stdio_filter_driver(stdio_driver_t *driver);
int stdio_put_string(const char *text, int length, bool newline, bool cr_translation);

#endif
