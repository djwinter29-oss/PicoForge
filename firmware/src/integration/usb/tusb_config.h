#ifndef PF_TUSB_CONFIG_H
#define PF_TUSB_CONFIG_H

#include "pico.h"
#include "tusb_option.h"

/* Pico SDK 2.3.0's TinyUSB DCD supports RP2350 through its RP2040 USB block. */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_TASK_QUEUE_SZ 16

#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64

#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64

#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512

#endif