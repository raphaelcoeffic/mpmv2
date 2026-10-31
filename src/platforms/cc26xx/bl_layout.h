#pragma once

// CC2652 flash layout shared by the boot loader (core/bootloader.c), the
// application build (boards/mpm_v2/app.ld) and tools/fw_image.py (see
// docs/update.md)
//
//   0x00000 - 0x01FFF  boot loader (sector 0)
//   0x02000 - 0x55FFF  application, vector table first
//   0x56000 - 0x57FFF  last sector: CCFG (boot loader image), never written
//                      by the boot loader

#if defined(MCU_CC26X0)
#error "No boot loader on the CC2650"
#endif

#define FLASH_SECTOR_SIZE 0x2000
#define APP_START         0x00002000
#define APP_END           0x00056000

#define SRAM_START 0x20000000
#define SRAM_END   0x20014000
