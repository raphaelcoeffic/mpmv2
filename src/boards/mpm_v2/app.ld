/* Application behind the boot loader: 0x2000 up to the last sector (CCFG,
   never written by the boot loader). See platforms/cc26xx/bl_layout.h. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x2000, LENGTH = 0x54000
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x14000
}

/* The length in the image header (core/app_header.c) */
__img_length = _eimage - ORIGIN(FLASH);

INCLUDE sections.ld
