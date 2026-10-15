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

enum { CONTROL_STAGE_SETUP, CONTROL_STAGE_DATA, CONTROL_STAGE_ACK };
enum { TUSB_REQ_TYPE_STANDARD, TUSB_REQ_TYPE_CLASS };
enum { MSC_REQ_RESET = 0xff, MSC_REQ_GET_MAX_LUN = 0xfe };
typedef struct {
 union { uint8_t bmRequestType; struct { uint8_t recipient:5, type:2, direction:1; } bmRequestType_bit; };
 uint8_t bRequest;
 uint16_t wValue, wIndex, wLength;
} tusb_control_request_t;
bool mscd_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
void mscd_reset(uint8_t);
