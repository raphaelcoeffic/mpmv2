#pragma once

// Flash layout shared by the boot loader, the application build
// (app.ld) and tools/fw_image.py (see docs/update.md)
//
//   0x00000 - 0x01FFF  boot loader (sector 0)
//   0x02000 - 0x55FFF  application, vector table first
//   0x56000 - 0x57FFF  last sector: CCFG (boot loader image), never written
//                      by the boot loader

#include <stdint.h>

#define FLASH_SECTOR_SIZE 0x2000
#define APP_START         0x00002000
#define APP_END           0x00056000

#define SRAM_START 0x20000000
#define SRAM_END   0x20014000

// Image header in the reserved vector table entries 7 to 10. The linker
// (app.ld) writes magic and length, no CRC (0xFFFFFFFF) and
// `checked` 0: an image written into flash as it is (debugger, ROM boot
// loader) starts without a CRC check. The file for the radio
// (tools/fw_image.py) has the CRC32 (zlib) of the image from APP_START over
// `length` bytes, the header itself read as zeros, and `checked` 0xFFFFFFFF;
// an STK500 update forces it to 0xFFFFFFFF too. The boot loader programs it
// to 0 once the CRC matched (first start after an update), later starts
// skip the CRC.
#define APP_HDR_OFFSET  0x1C
#define APP_HDR_MAGIC   0x3256504D  // "MPV2"
#define APP_HDR_CHECKED 0

typedef struct {
  uint32_t magic;
  uint32_t length;
  uint32_t crc;
  uint32_t checked;
} app_header_t;

// STK500 device signature: 0x1E (required by EdgeTX), then "U&" for the
// CC2652 (the STM32 MULTI boot loader answers 1E 55 AA)
#define BL_SIGNATURE_0 0x1E
#define BL_SIGNATURE_1 0x55
#define BL_SIGNATURE_2 0x26
