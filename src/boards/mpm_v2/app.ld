/* Application behind the boot loader: 0x2000 up to the last sector (CCFG,
   never written by the boot loader). See bootloader/layout.h. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x2000, LENGTH = 0x54000
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x14000
}

/* Image header (bootloader/layout.h): magic, length, no CRC, and
   `checked` already 0: written into flash as it is (debugger, ROM boot
   loader), the boot loader starts it without a CRC check. The file for the
   radio gets the CRC and `checked` 0xFFFFFFFF (tools/fw_image.py), as an
   STK500 update forces `checked` back anyway. */
__img_hdr_magic = 0x3256504D;   /* "MPV2" */
__img_hdr_length = _eimage - ORIGIN(FLASH);
__img_hdr_crc = 0xFFFFFFFF;
__img_hdr_checked = 0;

INCLUDE sections.ld
