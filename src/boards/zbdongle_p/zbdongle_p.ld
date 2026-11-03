/* ZBDongle-P (CC2652P, 352 KB): one image at 0x0, no boot loader of ours
   (the ROM boot loader flashes it, tools/rom_flash.py --sonoff), the CCFG
   at the end of the last sector. 80 KB of SRAM. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x0, LENGTH = 0x56000
    FLASH_CCFG (RX) : ORIGIN = 0x57FA8, LENGTH = 88
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x14000
}

INCLUDE sections.ld

SECTIONS
{
    .ccfg :
    {
        KEEP(*(.ccfg));
    } > FLASH_CCFG
}
