#pragma once

#include <driverlib/ioc.h>

// *******************************
// * FrSky XJT-Lite (CC2650F128) *
// *******************************
//
// No schematic: reverse engineered (stock firmware, OpenOCD, SDR)
//
// DIO_26 <-  module serial in: the bay's data pin (the MT12's PPM pin,
//            EdgeTX's USART6 TX) through an inverter (RX test pad).
//            MULTI idles high on the pin: low on DIO_26
// DIO_25  -> the bay's other pin (radio RX, PXX2) through an inverter
//            (TX test pad). Unused
//
// DIO_29  -> S.Port out: "C40R" inverting 3-state buffer, 68 ohm
// DIO_24  -> its output enable, active low (10k pull-up)
// DIO_30 <-  S.Port in
//
// DIO_6   -> SE2431L CSD: 1 = on (0 = sleep)
// DIO_5   -> SE2431L CPS: 1 = LNA (RX)
// DIO_4   -> SE2431L CTX: 1 = TX (PA, about 23 dB of gain)
//            ANT_SEL is strapped (one antenna). See doc/SE2431L_202410L.pdf
//
// DIO_11  -> red LED, DIO_13 -> green LED (1 = on). DIO_12: driven
//            with them by the stock app, no visible LED
//
// DIO_9  <-> I2C EEPROM SDA, DIO_10 -> SCL (open drain, board pull-ups):
//            256 bytes (M24C02 type) at 0x51
//
#define MODULE_RX     IOID_26
#define SPORT_TX      IOID_29
#define SPORT_TX_EN_N IOID_24
#define SPORT_RX      IOID_30
//
#define FEM_CSD       IOID_6
#define FEM_CPS       IOID_5
#define FEM_CTX       IOID_4
//
#define LED_RED       IOID_11
#define LED_GREEN     IOID_13
#define LED_UNKNOWN   IOID_12
//
#define EEPROM_SDA    IOID_9
#define EEPROM_SCL    IOID_10
#define EEPROM_I2C_ADDR 0x51
//
// RF core TX power word (CC2650 table), out of the antenna port through
// the PA (FrSky's table): 0x0CC7 9 dBm, 0x0CC9 12, 0x0CCB 14, 0x144B 15,
// 0x194E 16, 0x1D52 17, 0x2558 19, 0x3161 21. The stock default: 19 dBm
#define RF_TX_POWER_DEFAULT 0x2558
// Bind and range check: the lowest, 9 dBm
#define RF_TX_POWER_LOW     0x0CC7
// The MPM's low power flag: the lowest too, 10 dB lower
#define RF_TX_POWER_REDUCED 0x0CC7

void board_init();
// The board's pins at boot (board.c), from board_init()
void board_pins_init();
// The applications' part (core/main_*.c), after timer_init(): what the boot
// loader doesn't set up: nothing (the LEDs are pins)
void board_app_init();
