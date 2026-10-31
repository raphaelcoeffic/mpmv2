// LP-EM-CC2755P10 (CC27xx): LEDs, buttons, UART0 on the XDS110 back channel
// (115200 8N1), HFXT (48 MHz crystal). The chip's base: cc27xx.h

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cc27xx.h"

#define BOARD_LED_RED 16
#define BOARD_LED_GREEN 12
#define BOARD_BTN1 20
#define BOARD_BTN2 0
#define BOARD_UART_TX 1
#define BOARD_UART_RX 2
#define BOARD_UART_IOC 4  // UART0's function on DIO1 (TXD) and DIO2 (RXD)

// What the shared code takes from the board (as the CC2652 boards): the
// bench's UART on the board's (uart.c ignores them)
#define SERIAL_RX_IOD BOARD_UART_RX
#define SERIAL_TX_IOD BOARD_UART_TX
#define IOID_UNUSED 0xFFFFFFFF

// Boot loader link to the radio: UART0, the XDS110 back channel, where the
// MPM port talks to the radio (tools/bench/radio_sim.py): the MPM v2's
// window, up to 300 ms on an idle line, the application at once on radio
// traffic (docs/update.md)
#define BL_RADIO_UART0

// TX power: the CC2652's words, the platform's radio.c takes them as their
// dBm, or a dBm value itself (RF_TX_DBM()). The bench: +5 dBm as the CC2652
// LaunchPad
#define RF_TX_POWER_DEFAULT 0x7217
#define RF_TX_DBM(dbm) ((uint16_t)(uint8_t)(int8_t)(dbm))
// The MPM: the CC2755P10's +20 dBm PA, full power at +18 dBm as the MPM v2
// (docs/interop-results.md, TX power), the levels below it in dB
// (rf_power_word()) at the PHYs' power table entries (TI's dBm, not
// measured here): 1 dB steps down to 0 dBm, then 4 dB down to -20 dBm
#define RF_TX_POWER_STEPS                                                   \
  {0, RF_TX_DBM(18)}, {1, RF_TX_DBM(17)}, {2, RF_TX_DBM(16)},               \
  {3, RF_TX_DBM(15)}, {4, RF_TX_DBM(14)}, {5, RF_TX_DBM(13)},               \
  {6, RF_TX_DBM(12)}, {7, RF_TX_DBM(11)}, {8, RF_TX_DBM(10)},               \
  {9, RF_TX_DBM(9)}, {10, RF_TX_DBM(8)}, {11, RF_TX_DBM(7)},                \
  {12, RF_TX_DBM(6)}, {13, RF_TX_DBM(5)}, {14, RF_TX_DBM(4)},               \
  {15, RF_TX_DBM(3)}, {16, RF_TX_DBM(2)}, {17, RF_TX_DBM(1)},               \
  {18, RF_TX_DBM(0)}, {22, RF_TX_DBM(-4)}, {26, RF_TX_DBM(-8)},             \
  {30, RF_TX_DBM(-12)}, {34, RF_TX_DBM(-16)}, {38, RF_TX_DBM(-20)}

void board_led(uint32_t dio, bool on);
void board_led_toggle(uint32_t dio);
bool board_button(uint32_t dio);
