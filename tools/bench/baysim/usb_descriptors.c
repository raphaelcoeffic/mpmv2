// USB descriptors: a composite device with two CDC ports, "baysim link"
// (interface 0) and "baysim ctrl" (interface 2)

#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0x2E8A  // Raspberry Pi
#define USB_PID 0x4D32  // bench tool, not registered

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // CDC uses Interface Association Descriptors
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

enum { ITF_LINK, ITF_LINK_DATA, ITF_CTRL, ITF_CTRL_DATA, ITF_TOTAL };

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // interface, string, notification EP and size, data EPs (out, in) and size
    TUD_CDC_DESCRIPTOR(ITF_LINK, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(ITF_CTRL, 5, 0x83, 8, 0x04, 0x84, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *const strings[] = {
    NULL,  // 0: language, below
    "mpm-v2 bench",
    "baysim",
    NULL,  // 3: the flash unique id
    "baysim link",
    "baysim ctrl",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[1 + 32];
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *s;
    size_t n;

    if (index == 0) {
        desc[1] = 0x0409;
        n = 1;
    } else {
        if (index >= sizeof(strings) / sizeof(strings[0]))
            return NULL;
        s = strings[index];
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            s = serial;
        }
        n = strlen(s);
        if (n > 32)
            n = 32;
        for (size_t i = 0; i < n; i++)
            desc[1 + i] = (uint8_t)s[i];
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}
