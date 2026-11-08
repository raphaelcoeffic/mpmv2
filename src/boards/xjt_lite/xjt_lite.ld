/* FrSky XJT-Lite (CC2650F128): the application at __app_start (XJT_APP_START,
   board.cmake: 0x8000 behind FrSky's boot loader, 0x0 to 0x7FFF, kept: it
   starts the image at 0x8000, MSP and PC from its vector table). Up to
   0x1F000: the last sector holds the CCFG (and FrSky's 0x1FC00) and is
   never written. 20 KB of SRAM. */
MEMORY
{
    FLASH (RX) : ORIGIN = __app_start, LENGTH = 0x1F000 - __app_start
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x5000
}

INCLUDE sections.ld
