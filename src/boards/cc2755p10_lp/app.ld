/* CC27xx application behind the boot loader: 0x2000 up to the HSM firmware
   (the last 96 KB of flash, from 0xE8000). See platforms/cc27xx/bl_layout.h. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x2000, LENGTH = 0xE6000
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x24000
}

/* The length in the image header (core/app_header.c) */
__img_length = _eimage - ORIGIN(FLASH);

INCLUDE sections.ld
