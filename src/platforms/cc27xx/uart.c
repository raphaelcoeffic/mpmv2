// uart.h on the CC27xx (platforms/cc26xx/uart.c on the CC2652): UART0 on the
// XDS110 back channel through the board's board.c, polled; what the radio
// bench uses. The pins are the board's (uart_device_t's ignored)

#include "uart.h"

#include "board.h"

void uart_init(uart_t uart, const uart_device_t* dev)
{
  (void)uart;
  board_uart_init(dev->baud_rate);
}

void uart_print(uart_t uart, const char* str)
{
  (void)uart;
  while (*str) uart_putc(*str++);
}

void uart_put_char_blocking(uart_t uart, uint8_t data)
{
  (void)uart;
  uart_putc(data);
}

static int _rx = -1;

bool uart_rx_fifo_available(uart_t uart)
{
  (void)uart;
  if (_rx < 0) _rx = uart_getc();
  return _rx >= 0;
}

uint8_t uart_get_char(uart_t uart)
{
  if (!uart_rx_fifo_available(uart)) return 0;
  uint8_t c = _rx;
  _rx = -1;
  return c;
}
