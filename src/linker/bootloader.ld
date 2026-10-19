/* Boot loader: flash sector 0, and CCFG at the end of the last sector */
MEMORY
{
    FLASH (RX) : ORIGIN = 0x0, LENGTH = 0x2000
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
