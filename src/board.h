#pragma once

#if defined(BOARD_CC2652R1_LAUNCHXL)
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  #define LED_GREEN     IOID_7
  #define LED_RED       IOID_6
#else
  // *****************
  // * MPM v2 pinout *
  // *****************
  // 
  // DIO_3  <-> USB TX
  // DIO_2  <-> SPORT RX / USB RX
  // DIO_21 <-> SPORT TX
  // DIO_23 <-> SPORT RX INV
  // DIO_24 <-> SPORT TX INV
  // 
  // DIO_22 <-> CPPM
  // 
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  #define LED_DIN       IOID_18
  #define LED_SPI       SPI1

  // timing test: BOOT pin
  #define TEST_PIN      IOID_19
#endif

void board_init();
