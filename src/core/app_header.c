// The image header of an application behind the boot loader (app_image.h),
// right after the vector table (sections.ld). The board's app.ld gives the
// image length (__img_length)
#include "app_image.h"

extern const uint8_t __img_length[];

__attribute__((section(".img_hdr"), used)) const app_header_t _app_header = {
    .magic = APP_HDR_MAGIC,
    .chip = PLATFORM_CHIP_ID,
    .length = (uint32_t)__img_length,
    .crc = 0xFFFFFFFF,
    .checked = APP_HDR_CHECKED,
};
