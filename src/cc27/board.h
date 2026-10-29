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

void uart_init(uint32_t baudrate);
void uart_putc(uint8_t c);
void uart_write(const void *buf, uint32_t len);
int uart_getc(void);  // -1 if nothing received
void uart_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
