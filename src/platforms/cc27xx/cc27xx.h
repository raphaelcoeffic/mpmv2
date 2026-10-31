// The CC27xx's base (board.c): power-up, HFXT, the system timer's
// microseconds, UART0 as a polled console. The board's board.h includes it
// and gives its pins: BOARD_UART_TX / BOARD_UART_RX with BOARD_UART_IOC
// (their UART0 function), BOARD_HFXT_PPB (the crystal's offset)

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_ioc.h>

// Pads, HFXT, GPIO, then the board's pins (board_pins_init(), the board's
// board.c)
void board_init(void);
void board_pins_init(void);

// HFXT: started by board_init(), settled after some 100 us
bool board_hfxt_good(void);
bool board_hfxt_wait(uint32_t timeout_us);

// System timer: 1 us ticks, 32 bits
uint32_t board_us(void);
void board_delay_us(uint32_t us);

// UART0 on the board's pins, 8N1, polled
void board_uart_init(uint32_t baudrate);
void uart_putc(uint8_t c);
void uart_write(const void *buf, uint32_t len);
int uart_getc(void);  // -1 if nothing received
void uart_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// A pad's configuration (IOC_IOC0_*)
static inline void board_ioc(uint32_t dio, uint32_t cfg)
{
  HWREG(IOC_BASE + IOC_O_IOC0 + 4 * dio) = cfg;
}
