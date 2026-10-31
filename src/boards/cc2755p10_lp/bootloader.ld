/* CC27xx (CC2755P10) boot loader: flash sectors 0 to 3, and the CCFG
   (0x4E020000, the ROM checks its CRCs: tools/cc27_ccfg.py fills them in
   after the link). The SCFG is not in the image: the one on the device
   stays. 144 KB of SRAM (with parity). See platforms/cc27xx/bl_layout.h,
   from TI's cc27xxx10.lds. */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x0, LENGTH = 0x2000
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
