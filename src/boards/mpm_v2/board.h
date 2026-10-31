// MPM v2 (CC2652RB) and the CC2652R1 LaunchPad standing in for it: pins,
// TX power words, boot loader link

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
#else
  #error "No board: BOARD_MPM_V2 or BOARD_CC2652R1_LAUNCHXL from CMake's BOARD"
#endif

void board_init();
// The board's pins at boot (board.c), from board_init()
void board_pins_init();
