#pragma once

// CC27xx (CC2755P10) flash layout shared by the boot loader
// (core/bootloader.c), the applications' link (boards/cc2755p10_lp/app.ld)
// and tools/fw_image.py (see docs/update.md)
//
//   0x00000 - 0x01FFF  boot loader (sectors 0 to 3)
//   0x02000 - 0xE7FFF  application, vector table first
//   0xE8000 -          HSM firmware, never written
//   0x4E020000         CCFG (boot loader image), outside the main flash

#define FLASH_SECTOR_SIZE 0x800
#define APP_START         0x00002000
#define APP_END           0x000E8000

#define SRAM_START 0x20000000
#define SRAM_END   0x20024000
