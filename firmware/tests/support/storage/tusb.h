// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

#ifndef STORAGE_TEST_TUSB_H
#define STORAGE_TEST_TUSB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SCSI_SENSE_NOT_READY 2u
#define SCSI_SENSE_MEDIUM_ERROR 3u
#define SCSI_SENSE_ILLEGAL_REQUEST 5u
#define SCSI_SENSE_UNIT_ATTENTION 6u
#define CONTROL_STAGE_SETUP 0u
#define CONTROL_STAGE_DATA 1u
#define CONTROL_STAGE_ACK 2u
#define TUSB_REQ_TYPE_CLASS 1u
#define TUSB_DIR_OUT 0u
#define HID_REQ_CONTROL_SET_REPORT 9u
#define MSC_REQ_RESET 0xffu
typedef enum { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_FEATURE = 3 } hid_report_type_t;
typedef enum { XFER_RESULT_SUCCESS = 0 } xfer_result_t;
typedef struct {
    struct { uint8_t type, direction; } bmRequestType_bit;
    uint8_t bRequest;
    uint16_t wValue, wLength;
} tusb_control_request_t;
bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request, void *buffer, uint16_t length);
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
void tud_task_ext(uint32_t timeout_ms, bool in_isr);
bool tusb_init(void);
#endif
