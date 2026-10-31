/* CC27xx (CC2755P10): the application at 0 (no boot loader), up to the HSM
   firmware (the last 96 KB of flash, from 0xE8000), 144 KB of SRAM (with
   parity), and the CCFG (0x4E020000, the ROM checks its CRCs:
   tools/cc27_ccfg.py fills them in after the link). The SCFG is not in the
   image: the one on the device stays. From TI's cc27xxx10.lds. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x0, LENGTH = 0xE8000
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x24000
    CCFG (R) : ORIGIN = 0x4E020000, LENGTH = 0x800
}

INCLUDE sections.ld

SECTIONS
{
    .ccfg :
    {
        KEEP(*(.ccfg));
    } > CCFG
}
