#ifndef STORAGE_TEST_UNIQUE_ID_H
#define STORAGE_TEST_UNIQUE_ID_H
#include <stddef.h>
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8u
void pico_get_unique_board_id_string(char *id, size_t size);
#endif
