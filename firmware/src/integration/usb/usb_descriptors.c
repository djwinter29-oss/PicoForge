#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_status.h"

#define USB_VID 0xCAFEu
#define USB_PID 0x4011u
#define USB_BCD_DEVICE 0x0100u

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_HID_IN 0x83
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

/* Development-only vendor-defined reports; replace the VID/PID before release. */
static uint8_t const hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x85, PICOFORGE_HID_INFO_REPORT_ID,
    0x95, PICOFORGE_HID_INFO_PAYLOAD_SIZE,
    0x09, 0x01,
    0xB1, 0x02,
    0x85, PICOFORGE_HID_STATUS_REPORT_ID,
    0x95, PICOFORGE_HID_STATUS_PAYLOAD_SIZE,
    0x09, 0x02,
    0x81, 0x02,
    0xC0,
};

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), EPNUM_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "PicoForge",
    "PicoForge CDC + HID",
    NULL,
    "CDC ACM",
    "Vendor HID",
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

        pico_get_unique_board_id_string(serial, sizeof(serial));
        chr_count = strlen(serial);
        for (size_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)(unsigned char)serial[i];
        }
    } else {
        if ((index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) ||
            (string_desc_arr[index] == NULL)) {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        for (size_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)(unsigned char)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}