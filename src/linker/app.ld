/* Application behind the boot loader: 0x2000 up to the last sector (CCFG,
   never written by the boot loader). See src/bootloader/layout.h. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x2000, LENGTH = 0x54000
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x14000
}

INCLUDE sections.ld
