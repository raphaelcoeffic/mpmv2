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
  // timing test: BOOT pin
  #define TEST_PIN      IOID_19
  //
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
  #define BUTTON        IOID_0
  //
  // timing test: BOOT pin
  #define TEST_PIN      IOID_19
  //
#endif

void board_init();
