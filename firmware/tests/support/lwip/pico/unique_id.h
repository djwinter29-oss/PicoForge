#ifndef PF_TEST_PICO_UNIQUE_ID_H
#define PF_TEST_PICO_UNIQUE_ID_H

#include <stdint.h>

#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8u

typedef struct {
    uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
} pico_unique_board_id_t;

void pico_get_unique_board_id(pico_unique_board_id_t *id);

#endif
