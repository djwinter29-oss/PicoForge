#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "hid.h"
#include "msc.h"

#define USB_VID 0xCAFEu
#define USB_PID 0x4011u

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA = 1,
    ITF_NUM_HID = 2,
    ITF_NUM_MSC = 3,
    ITF_NUM_TOTAL_NO_MSC = 3,
    ITF_NUM_TOTAL_MSC = 4,
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_HID,
    STRID_MSC,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u
#define EPNUM_HID_IN 0x83u
#define EPNUM_MSC_OUT 0x04u
#define EPNUM_MSC_IN 0x84u
#define CONFIG_TOTAL_LEN_NO_MSC (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define CONFIG_TOTAL_LEN_MSC \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x85, PF_HID_INFO_REPORT_ID,
    0x95, PF_HID_INFO_PAYLOAD_SIZE,
    0x09, 0x01,
    0xB1, 0x02,
    0x85, PF_HID_STATUS_REPORT_ID,
    0x95, PF_HID_STATUS_PAYLOAD_SIZE,
    0x09, 0x02,
    0x81, 0x02,
    0x85, PF_HID_MEMORY_REPORT_ID,
    0x95, PF_HID_MEMORY_PAYLOAD_SIZE,
    0x09, 0x03,
    0x81, 0x02,
    0x85, PF_HID_LED_COMMAND_REPORT_ID,
    0x95, PF_HID_LED_COMMAND_PAYLOAD_SIZE,
    0x09, 0x04,
    0xB1, 0x02,
    0x85, PF_HID_MSC_COMMAND_REPORT_ID,
    0x95, PF_HID_MSC_STATUS_PAYLOAD_SIZE,
    0x09, 0x05,
    0x81, 0x02,
    0x95, PF_HID_MSC_COMMAND_PAYLOAD_SIZE,
    0x09, 0x05,
    0xB1, 0x02,
    0x85, PF_HID_SELF_TEST_REPORT_ID,
    0x95, PF_HID_SELF_TEST_STATUS_PAYLOAD_SIZE,
    0x09, 0x06,
    0x81, 0x02,
    0x95, PF_HID_SELF_TEST_COMMAND_PAYLOAD_SIZE,
    0x09, 0x06,
    0xB1, 0x02,
    0x85, PF_HID_FILESYSTEM_REPORT_ID,
    0x95, PF_HID_FILESYSTEM_PAYLOAD_SIZE,
    0x09, 0x07,
    0x81, 0x02,
    0xC0,
};

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

static uint8_t const desc_configuration_no_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_NO_MSC, 0, CONFIG_TOTAL_LEN_NO_MSC, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), EPNUM_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

static uint8_t const desc_configuration_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_MSC, 0, CONFIG_TOTAL_LEN_MSC, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), EPNUM_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

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

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "PicoForge",
    "PicoForge CDC + HID",
    NULL,
    "CDC ACM",
    "Vendor HID",
    "SD Card",
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    const char *string;
    size_t char_count;
    char serial[2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1u];

    (void)langid;
    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], string_desc_arr[0], 2u);
        char_count = 1u;
    } else if (index == STRID_SERIAL) {
        pico_get_unique_board_id_string(serial, sizeof(serial));
        string = serial;
        char_count = strlen(string);
        for (size_t i = 0u; i < char_count; i++) {
            desc_str[1u + i] = (uint16_t)(unsigned char)string[i];
        }
    } else {
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ||
            string_desc_arr[index] == NULL) {
            return NULL;
        }
        string = string_desc_arr[index];
        char_count = strlen(string);
        for (size_t i = 0u; i < char_count; i++) {
            desc_str[1u + i] = (uint16_t)(unsigned char)string[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8u) | (2u * char_count + 2u));
    return desc_str;
}