
#if defined(MCU_CC2652RB)
  // HF source is HPOSC (BAW) (only valid for CC2652RB)
  #define SET_CCFG_MODE_CONF_XOSC_FREQ 0x1
#else
  // HF source is a 48 MHz xtal
  #define SET_CCFG_MODE_CONF_XOSC_FREQ 0x2
#endif

// Enable ROM boot loader
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5

// Enabled boot loader backdoor
#define SET_CCFG_BL_CONFIG_BL_ENABLE 0xC5

// DIO number for boot loader backdoor: DIO_19
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER 19

// Active low to open boot loader backdoor
#define SET_CCFG_BL_CONFIG_BL_LEVEL 0x0

#include "startup_files/ccfg.c"
