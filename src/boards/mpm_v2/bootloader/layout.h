#pragma once

// Flash layout shared by the boot loader, the application build
// (app.ld) and tools/fw_image.py (see docs/update.md)
//
//   0x00000 - 0x01FFF  boot loader (sector 0)
//   0x02000 - 0x55FFF  application, vector table first
//   0x56000 - 0x57FFF  last sector: CCFG (boot loader image), never written
//                      by the boot loader

#include <stdint.h>

#include "app_image.h"

#define FLASH_SECTOR_SIZE 0x2000
#define APP_START         0x00002000
#define APP_END           0x00056000

#define SRAM_START 0x20000000
#define SRAM_END   0x20014000

// The image header after the application's vector table: app_image.h

// STK500 device signature: 0x1E (required by EdgeTX), then "U&" for the
// CC2652 (the STM32 MULTI boot loader answers 1E 55 AA)
#define BL_SIGNATURE_0 0x1E
#define BL_SIGNATURE_1 0x55
#define BL_SIGNATURE_2 0x26
