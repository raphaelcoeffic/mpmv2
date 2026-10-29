#pragma once

#include <driverlib/ioc.h>

#if defined(BOARD_CC2652R1_LAUNCHXL)
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  //
  #define LED_GREEN     IOID_7
  #define LED_RED       IOID_6
  //
  #define LED_DIN       IOID_18
  #define LED_SPI       SPI1
  //
  #define FLASH_MISO    IOID_8
  #define FLASH_MOSI    IOID_9
  #define FLASH_SCLK    IOID_10
  #define FLASH_CS      IOID_20
  #define FLASH_SPI     SPI0
  //
  #define BUTTON        IOID_13
  //
  // RF core TX power word: +5 dBm (TI table)
  #define RF_TX_POWER_DEFAULT 0x7217
  // Bind and range check (the NRF24L01's -18 dBm): 25 dB lower
  #define RF_TX_POWER_LOW     0x04C6
  // The MPM's low power flag: -5 dBm, 10 dB lower (TI table)
  #define RF_TX_POWER_REDUCED 0x12D6
  // FrSky: option 0 at stock FrSky gear's frequency, 25 FSCTRL0 steps
  // (40 kHz) below the crystal's nominal one (mpm/upstream/cc2500.c)
  #define BOARD_FRSKY_FREQ_STEPS -25
  //
  // timing test: BOOT pin
  #define TEST_PIN      IOID_19
  //
#elif defined(BOARD_MPM_V2)
  // *****************
  // * MPM v2 pinout *
  // *****************
  //
  // See doc/MPM_OUT.pdf (rev 1.3)
  //
  // DIO_2  <-> U8 mux: USB RX (CH343 TXD) when USB is plugged,
  //            SPORT RX (through U9 XOR with RX_INV) otherwise
  // DIO_3  <-> USB TX (CH343 RXD)
  //
  // DIO_21 <-> SPORT TX (through U9 XOR with TX1_INV, open drain on SPORT)
  // DIO_22 <-> CPPM / module serial in (JR bay pin 1)
  // DIO_23 <-> SPORT RX INV (U9 2B)
  // DIO_24 <-> SPORT TX INV (U9 1B)
  //
  // DIO_13 <-> RF switch U3: 1 = CC2652, 0 = SX1281
  // DIO_14 <-> SKY66112 PA/LNA CTX: 1 = TX (high power), 0 = RX (LNA)
  //            (CSD = CRX = CHL = 1 and CPS = 0 are hardwired)
  // DIO_15 <-> SKY66112 ANT_SEL: 0 = ANT1 (J2), 1 = ANT2 (J3)
  //
  // SX1281 shares the SPI bus with the flash
  //
  // DIO_19 <-> BOOT: ROM boot loader backdoor (R15 pull-up, Q1 from CH343)
  //
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  //
  #define MODULE_RX     IOID_22
  #define MODULE_TX     IOID_21
  #define MODULE_RX_INV IOID_23
  #define MODULE_TX_INV IOID_24
  //
  #define LED_DIN       IOID_18
  #define LED_SPI       SPI1
  //
  #define FLASH_MISO    IOID_8
  #define FLASH_MOSI    IOID_9
  #define FLASH_SCLK    IOID_10
  #define FLASH_CS      IOID_7
  #define FLASH_SPI     SPI0
  //
  #define SX1281_NSS    IOID_11
  #define SX1281_NRST   IOID_26
  #define SX1281_DIO1   IOID_27
  #define SX1281_BUSY   IOID_28
  #define SX1281_SPI    FLASH_SPI
  //
  #define RF_SW         IOID_13
  #define RF_SW_CC2652  1
  #define RF_SW_SX1281  0
  //
  #define PA_CTX        IOID_14
  #define ANT_SEL       IOID_15
  //
  // RF core TX power word: 0 dBm (CC2652RB table). The SKY66112 input
  // is +5 dBm max, -2 dBm recommended, 22 dB gain (+21 dBm out):
  // see doc/SKY66112-11_203225P.pdf
  #define RF_TX_POWER_DEFAULT 0x2853
  // Bind and range check (the NRF24L01's -18 dBm): about 20 dB lower
  #define RF_TX_POWER_LOW     0x04C6
  // The MPM's low power flag: -10 dBm into the PA, 10 to 13 dB lower (a
  // stock X8R's RSSI 10.5 below the default's, docs/interop-results.md)
  #define RF_TX_POWER_REDUCED 0x0ACF
  // FrSky: option 0 at stock FrSky gear's frequency, 25 FSCTRL0 steps
  // (40 kHz) below the HPOSC's corrected one (mpm/upstream/cc2500.c)
  #define BOARD_FRSKY_FREQ_STEPS -25
  //
  #define BUTTON        IOID_0
  //
  // Boot loader link to the radio (EdgeTX "Flash External Multi", STK500
  // at 57600 8N1, see docs/update.md): in on the PPM pin, out on S.Port
  // through U9 (TX1_INV), both idle high (EdgeTX's ETX_Pol_Inverted: the
  // bay's "normal" is idle low). No BL_RADIO_ABSENT_LEVEL: the radio's idle
  // level is R14's
  #define BL_RADIO_RX       MODULE_RX
  #define BL_RADIO_TX       MODULE_TX
  #define BL_RADIO_INVERTED 0
  //
#elif defined(BOARD_XJT_LITE)
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
  // FrSky: option 0 at stock FrSky gear's frequency, 25 FSCTRL0 steps
  // (40 kHz) below the crystal's nominal one, as on the MPM v2 (a stock
  // CC2650 receiver needs -25 against this board too; mpm/upstream/cc2500.c)
  #define BOARD_FRSKY_FREQ_STEPS -25
  //
#else
  #error "No board: BOARD_<name> from CMake's BOARD"
#endif

void board_init();
