/** @file
 * @brief USB device descriptors: CDC CLI, vendor HID and the optional MSC interface.
 */
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_hid.h"
#include "usb_msc.h"
#if PF_ENABLE_WIRELESS
#include "app/wireless/wireless.h"
#endif

/* ponytail: development IDs; replace with an allocated VID/PID before distributing devices. The identity report's
 * PF_HID_PRODUCT_ID is an independent firmware-product field, not this USB PID. */
#define USB_VID 0xCAFEu /**< USB vendor ID. */
#define USB_PID 0x4011u /**< USB product ID. */

/** Interface numbers; MSC is last so the no-MSC configuration is a prefix. */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA = 1,
    ITF_NUM_HID = 2,
    ITF_NUM_MSC = 3,
    ITF_NUM_TOTAL_NO_MSC = 3,
    ITF_NUM_TOTAL_MSC = 4,
};

/** String descriptor indexes. */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID,
    STRID_MSC,
};

/** Endpoint addresses: bit 7 set is IN. */
#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u
#define EPNUM_HID_IN 0x83u
#define EPNUM_MSC_OUT 0x04u
#define EPNUM_MSC_IN 0x84u
/** Total configuration descriptor length without and with the MSC interface. */
#define CONFIG_TOTAL_LEN_NO_MSC (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define CONFIG_TOTAL_LEN_MSC \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)

/** Vendor HID report descriptor: feature 1, 2 and 20, input 3, 10 and 11, sized from usb_hid.h. */
static uint8_t const hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x85, PF_HID_IDENTITY_REPORT_ID,
    0x95, PF_HID_IDENTITY_PAYLOAD_SIZE,
    0x09, 0x01,
    0xB1, 0x02,
    0x85, PF_HID_CONFIGURATION_REPORT_ID,
    0x95, PF_HID_CONFIGURATION_PAYLOAD_SIZE,
    0x09, 0x02,
    0xB1, 0x02,
    0x85, PF_HID_TELEMETRY_REPORT_ID,
    0x95, PF_HID_TELEMETRY_PAYLOAD_SIZE,
    0x09, 0x03,
    0x81, 0x02,
    0x85, PF_HID_STORAGE_REPORT_ID,
    0x95, PF_HID_STORAGE_PAYLOAD_SIZE,
    0x09, 0x0A,
    0x81, 0x02,
    0x85, PF_HID_SELF_TEST_REPORT_ID,
    0x95, PF_HID_SELF_TEST_PAYLOAD_SIZE,
    0x09, 0x0B,
    0x81, 0x02,
    0x85, PF_HID_CONTROL_REPORT_ID,
    0x95, PF_HID_CONTROL_PAYLOAD_SIZE,
    0x09, 0x14,
    0xB1, 0x02,
#if PF_ENABLE_WIRELESS
    0x85, PF_WIRELESS_REPORT_ID,
    0x95, PF_WIRELESS_STATUS_SIZE,
    0x09, 0x1E,
    0xB1, 0x02,
    0x85, PF_WIRELESS_CONFIG_REPORT_ID,
    0x95, PF_WIRELESS_REQUEST_SIZE,
    0x09, 0x1F,
    0xB1, 0x02,
#endif
    0xC0,
};

/** Composite device (CDC + HID [+ MSC]) via IAD; USB 2.0 full speed. */
static tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = PF_BCD_DEVICE,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/** CDC and HID interfaces shared by both configurations, so an endpoint change cannot be made in only one. */
#define CDC_HID_DESCRIPTORS \
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64), \
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_descriptor), \
                       EPNUM_HID_IN, 64, 10)

/** Configuration while firmware owns the card. */
static uint8_t const desc_configuration_no_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_NO_MSC, 0, CONFIG_TOTAL_LEN_NO_MSC, 0x00, 100),
    CDC_HID_DESCRIPTORS,
};

/** Configuration while the USB host owns the card. */
static uint8_t const desc_configuration_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_MSC, 0, CONFIG_TOTAL_LEN_MSC, 0x00, 100),
    CDC_HID_DESCRIPTORS,
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

/**
 * Returns the configuration matching the current MSC ownership. The host only re-reads it after the USB
 * detach/reconnect that pf_usb_msc_task() performs on every ownership change, so the two always agree.
 */
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return pf_usb_msc_is_enabled() ? desc_configuration_msc : desc_configuration_no_msc;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

/* The language ID (index 0) is built in the callback; STRID_SERIAL is generated from the board ID. The product
 * string is matched by host tools and docs (host/python device.py), so it stays "CDC + HID" in both configurations. */
static char const *string_desc_arr[] = {
    NULL,
    "PicoForge",
    "PicoForge CDC + HID",
    NULL,
    "CDC ACM",
    "Vendor HID",
    "SD Card",
};

#define DESC_STR_MAX_CHARS 32u /**< Longest string descriptor payload, in UTF-16 code units. */
static uint16_t desc_str[DESC_STR_MAX_CHARS + 1u]; /**< Header word plus the string; shared, USB task only. */

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    const char *string = NULL;
    size_t char_count = 1u;
    char serial[2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1u];

    (void)langid;
    if (index == STRID_LANGID) {
        desc_str[1] = 0x0409u; /* English (US) */
    } else if (index == STRID_SERIAL) {
        pico_get_unique_board_id_string(serial, sizeof(serial));
        string = serial;
    } else if (index < (sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) && string_desc_arr[index] != NULL) {
        string = string_desc_arr[index];
    } else {
        return NULL;
    }
    if (string != NULL) {
        char_count = strlen(string);
        if (char_count > DESC_STR_MAX_CHARS) {
            char_count = DESC_STR_MAX_CHARS;
        }
        for (size_t i = 0u; i < char_count; i++) {
            desc_str[1u + i] = (uint16_t)(unsigned char)string[i];
        }
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8u) | (2u * char_count + 2u));
    return desc_str;
}
