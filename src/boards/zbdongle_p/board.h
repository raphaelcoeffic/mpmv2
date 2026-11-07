// Sonoff Zigbee 3.0 USB Dongle Plus, ZBDongle-P (CC2652P): pins, TX power,
// CCFG. The radio bench on it (a FASST sniffer: bench fasstrx), or the MPM
// port (mpm_port.c)

#pragma once

#include <driverlib/ioc.h>

// ************************
// * ZBDongle-P pinout    *
// ************************
//
// From the adapter table of Koenkk's Z-Stack firmware (the "launchpad"
// CC1352P2_CC2652P build runs on it) and Sonoff's own image (CCFG):
//
// DIO_12 <-> UART RX (CP2102N TXD)
// DIO_13 <-> UART TX (CP2102N RXD)
// DIO_18 / DIO_19 <-> RTS / CTS, only with the DIP switch on: unused
//
// DIO_15 <-> BOOT: ROM boot loader backdoor, active low, from the CP2102N's
//            DTR; its RTS resets the chip (tools/rom_flash.py --sonoff)
//
// DIO_29 <-> RF switch: 1 = the 20 dBm PA output, 0 = the normal RF path
//            (RX and the +5 dBm PA). No DIO_28 / DIO_30 switch lines, unlike
//            TI's CC1352P-2 LaunchPad
//
// No LED the firmware drives (DIO_7: not fitted), the red one is power

#define SERIAL_RX_IOD IOID_12
#define SERIAL_TX_IOD IOID_13

#define RF_HIGH_PA    IOID_29

// RF core TX power word, the normal PA path: +5 dBm in the CC2652R's table,
// not measured on the CC2652P (the 20 dBm PA needs its own setup: unused)
#define RF_TX_POWER_DEFAULT 0x7217
// The words for each level below it, dB (rf_power_word()): the CC2652R1
// LaunchPad's (measured there), not measured on the CC2652P
#define RF_TX_POWER_STEPS                                                    \
  {0, 0x7217}, {1, 0x4E63}, {2, 0x385D}, {3, 0x3259}, {5, 0x2853},           \
  {7, 0x1893}, {9, 0x12D6}, {11, 0x10D4}, {13, 0x0ACF}, {16, 0x0ACC},        \
  {19, 0x06CA}, {20, 0x0443}, {22, 0x0484}, {24, 0x04C6}, {26, 0x04C5},      \
  {28, 0x04C4}, {31, 0x04C3}, {34, 0x04C2}, {37, 0x04C1}

void board_init();
// The board's pins at boot (board.c), from board_init()
void board_pins_init();
// The applications' part (core/main_*.c): nothing to set up
void board_app_init();

// HF source is a 48 MHz xtal, LF a 32.768 kHz one; the cap-array trim of
// Sonoff's image (MODE_CONF 0xFDB9C13A, as the launchpad build:
// XOSC_CAPARRAY_DELTA 0xC1, applied; VDDR_CAP 0x3A there, 0xFF here: not
// used on the CC26x2)
#define SET_CCFG_MODE_CONF_XOSC_FREQ 0x2
#define SET_CCFG_MODE_CONF_SCLK_LF_OPTION 0x2
#define SET_CCFG_MODE_CONF_XOSC_CAP_MOD 0x0
#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xC1

// Power as in Sonoff's image: VDDR forced to its HH trim ("boost mode", the
// higher output powers; MODE_CONF_1 ALT_DCDC_VMIN 0xC, MODE_CONF
// VDDR_EXT_LOAD 0) and the DC/DC off, active and recharge
#define CCFG_FORCE_VDDR_HH 0x1
#define SET_CCFG_MODE_CONF_DCDC_ACTIVE 0x1
#define SET_CCFG_MODE_CONF_DCDC_RECHARGE 0x1

// ROM boot loader and its backdoor on DIO_15, active low: BL_CONFIG
// 0xC5FE0FC5 as in Sonoff's image. Anything else and only a debugger can
// flash the dongle again
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER 15
#define SET_CCFG_BL_CONFIG_BL_LEVEL 0x0
