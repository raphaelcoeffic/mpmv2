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
// dBm. +5 dBm as the CC2652 LaunchPad
#define RF_TX_POWER_DEFAULT 0x7217
// The words for each level below it, dB (rf_power_word()): TI's dBm, not
// measured here, down to its PHYs' lowest, -20 dBm
#define RF_TX_POWER_STEPS                                                       \
  {0, 0x7217}, {1, 0x4E63}, {2, 0x385D}, {3, 0x3259}, {4, 0x2856}, {5, 0x2853}, \
  {8, 0x1893}, {10, 0x12D6}, {11, 0x10D4}, {15, 0x0ACF}, {17, 0x0ACC},          \
  {20, 0x06CA}, {23, 0x06C8}, {25, 0x04C6}

void board_led(uint32_t dio, bool on);
void board_led_toggle(uint32_t dio);
bool board_button(uint32_t dio);
