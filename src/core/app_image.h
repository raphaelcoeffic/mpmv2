#pragma once

// The image of an application behind the boot loader (docs/update.md): its
// vector table first, then the image header (app_header.c, section
// .img_hdr, which sections.ld puts right after the vector table). The
// header's offset is the chip's vector table size: the boot loader and
// tools/fw_image.py find it by its magic in the first APP_HDR_SEARCH bytes,
// as EdgeTX's boot loader finds its version tag.
//
// The linker gives the length, no CRC (0xFFFFFFFF) and `checked` 0: an
// image written into flash as it is (debugger, ROM boot loader) starts
// without a CRC check. The file for the radio (tools/fw_image.py) has the
// CRC32 (zlib) of the image over `length` bytes, the header itself read as
// zeros, and `checked` 0xFFFFFFFF; an STK500 update forces it to 0xFFFFFFFF
// too. The boot loader programs it to 0 once the CRC matched (first start
// after an update), later starts skip the CRC.

#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define APP_HDR_MAGIC   0x494D504D  // "MPMI"
#define APP_HDR_CHECKED 0
// The header within the first bytes of the image: after the 16 system
// vectors, before this
#define APP_HDR_MIN    0x40
#define APP_HDR_SEARCH 0x200

typedef struct {
  uint32_t magic;
  uint32_t chip;  // PLATFORM_CHIP_ID (platform.h): the chip it was built for
  uint32_t length;
  uint32_t crc;
  uint32_t checked;
} app_header_t;

// The header's offset in the image's first len bytes, or 0
static inline uint32_t app_header_offset(const uint8_t* img, uint32_t len)
{
  if (len > APP_HDR_SEARCH) len = APP_HDR_SEARCH;
  for (uint32_t off = APP_HDR_MIN; off + sizeof(app_header_t) <= len; off += 4) {
    if (((const app_header_t*)(img + off))->magic == APP_HDR_MAGIC) return off;
  }
  return 0;
}
