#ifndef PF_TEST_DCD_H
#define PF_TEST_DCD_H

#include <stdbool.h>
#include <stdint.h>

#define CFG_TUSB_OS 5
#define OPT_OS_PICO 5
#define CFG_TUD_TASK_QUEUE_SZ 16u
#define DCD_EVENT_SOF 3u
#define DCD_EVENT_SETUP_RECEIVED 6u

typedef struct { uint8_t role; } tusb_rhport_init_t;
typedef struct { uint8_t address; } tusb_desc_endpoint_t;
typedef struct { uint8_t request; } tusb_control_request_t;
typedef struct {
    uint8_t event_id;
    uint32_t frame;
} dcd_event_t;

#endif
