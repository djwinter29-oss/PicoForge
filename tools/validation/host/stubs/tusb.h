#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_SENSE_MEDIUM_ERROR 3
bool tud_msc_set_sense(uint8_t, uint8_t, uint8_t, uint8_t);
static inline bool tud_disconnect(void) { return true; }
static inline bool tud_connect(void) { return true; }
