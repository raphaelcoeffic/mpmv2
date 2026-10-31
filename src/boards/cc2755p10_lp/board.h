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

// TX power: the CC2652's words, the platform's radio.c takes them as their
// dBm. +5 dBm as the CC2652 LaunchPad
#define RF_TX_POWER_DEFAULT 0x7217
#define RF_TX_POWER_LOW 0x04C6
// The MPM's low power flag: 15 dB lower
#define RF_TX_POWER_REDUCED 0x0ACF

void board_led(uint32_t dio, bool on);
void board_led_toggle(uint32_t dio);
bool board_button(uint32_t dio);
