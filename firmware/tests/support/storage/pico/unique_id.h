#ifndef STORAGE_TEST_UNIQUE_ID_H
#define STORAGE_TEST_UNIQUE_ID_H
#include <stddef.h>
#include <stdint.h>
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8u
typedef struct {
    uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
} pico_unique_board_id_t;
void pico_get_unique_board_id(pico_unique_board_id_t *id);
void pico_get_unique_board_id_string(char *id, size_t size);
#endif
