#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SCSI_SENSE_NOT_READY 2
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define SCSI_SENSE_MEDIUM_ERROR 3
#define SCSI_SENSE_UNIT_ATTENTION 6
bool tud_msc_set_sense(uint8_t, uint8_t, uint8_t, uint8_t);
extern unsigned test_usb_disconnects, test_usb_connects;
static inline bool tud_disconnect(void) { ++test_usb_disconnects; return true; }
static inline bool tud_connect(void) { ++test_usb_connects; return true; }
void tud_mount_cb(void);
void tud_umount_cb(void);
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
