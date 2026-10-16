#ifndef STORAGE_TEST_TUSB_H
#define STORAGE_TEST_TUSB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCSI_SENSE_NOT_READY 2u
#define SCSI_SENSE_MEDIUM_ERROR 3u
#define SCSI_SENSE_ILLEGAL_REQUEST 5u
#define CONTROL_STAGE_SETUP 0u
#define TUSB_REQ_TYPE_CLASS 1u
#define MSC_REQ_RESET 0xffu
typedef enum { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_FEATURE = 3 } hid_report_type_t;
typedef struct {
    struct { uint8_t type; } bmRequestType_bit;
    uint8_t bRequest;
    uint16_t wValue, wLength;
} tusb_control_request_t;
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq);
bool tud_disconnect(void);
bool tud_connect(void);
bool tud_mounted(void);
bool tud_cdc_connected(void);
bool tud_hid_ready(void);
bool tud_hid_report(uint8_t id, const void *report, uint16_t size);
uint32_t tud_cdc_available(void);
uint32_t tud_cdc_read(void *buffer, uint32_t size);
void tud_cdc_read_flush(void);
uint32_t tud_cdc_write(const void *buffer, uint32_t size);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_write_flush(void);
bool tud_cdc_write_clear(void);
void tud_task(void);
bool tusb_init(void);
#endif
