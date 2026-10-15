#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_SENSE_MEDIUM_ERROR 3
bool tud_msc_set_sense(uint8_t, uint8_t, uint8_t, uint8_t);
static inline bool tud_disconnect(void) { return true; }
static inline bool tud_connect(void) { return true; }
void tud_mount_cb(void);
void tud_umount_cb(void);
enum { DCD_EVENT_BUS_RESET = 1, DCD_EVENT_UNPLUGGED = 2 };
void tud_event_hook_cb(uint8_t, uint32_t, bool);
