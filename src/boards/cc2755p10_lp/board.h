// LP-EM-CC2755P10 (CC27xx): LEDs, buttons, UART0 on the XDS110 back channel
// (115200 8N1), the system timer, HFXT (48 MHz crystal)

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOARD_LED_RED 16
#define BOARD_LED_GREEN 12
#define BOARD_BTN1 20
#define BOARD_BTN2 0
#define BOARD_UART_TX 1
#define BOARD_UART_RX 2

// What the shared code takes from the board (src/board.h): the bench's
// UART on the board's (uart.c ignores them)
#define SERIAL_RX_IOD BOARD_UART_RX
#define SERIAL_TX_IOD BOARD_UART_TX
#define IOID_UNUSED 0xFFFFFFFF

// TX power: the CC2652's words, the platform's radio.c takes them as their
// dBm. +5 dBm as the CC2652 LaunchPad
#define RF_TX_POWER_DEFAULT 0x7217
#define RF_TX_POWER_LOW 0x04C6
// The MPM's low power flag: 15 dB lower
#define RF_TX_POWER_REDUCED 0x0ACF

void board_init(void);
void board_led(uint32_t dio, bool on);
void board_led_toggle(uint32_t dio);
bool board_button(uint32_t dio);

// HFXT: started by board_init(), settled after some 100 us
bool board_hfxt_good(void);
bool board_hfxt_wait(uint32_t timeout_us);

// System timer: 1 us ticks, 32 bits
uint32_t board_us(void);
void board_delay_us(uint32_t us);

void board_uart_init(uint32_t baudrate);
void uart_putc(uint8_t c);
void uart_write(const void *buf, uint32_t len);
int uart_getc(void);  // -1 if nothing received
void uart_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
