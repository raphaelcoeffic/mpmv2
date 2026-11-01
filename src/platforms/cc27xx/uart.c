// uart.h on the CC27xx (platforms/cc26xx/uart.c on the CC2652): UART0
// (board.c, cc27xx.h), polled as the radio bench uses it, or in interrupts
// as the MPM port does (no DMA). The pins are the board's (uart_device_t's
// ignored).
//
// RX in interrupts as on the CC2652: the FIFO level interrupt leaves bytes
// in the FIFO, the receive timeout (32 bit times of silence) takes them and
// ends the frame (frame_received). RX errors: the error interrupts and the
// error bits of each byte. TX from a ring, refilled by the interrupt.

#include "uart.h"

#include <driverlib/interrupt.h>
#include <driverlib/uart.h>
#include <inc/hw_ints.h>
#include <inc/hw_uart.h>

#include "board.h"

#define UART_ERRORS (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)
#define UART_DR_ERRORS (UART_DR_OE_M | UART_DR_BE_M | UART_DR_PE_M | UART_DR_FE_M)

// The RX interrupt at half the FIFO (4 bytes at least): it takes RX_IRQ_BYTES,
// one stays for the receive timeout
#define RX_IRQ_BYTES 3

void uart_init(uart_t uart, const uart_device_t* dev)
{
  (void)uart;
  board_uart_init(dev->baud_rate);
  if (dev->mode == UART_8E2) {
    UARTDisable(UART0_BASE);
    UARTConfigSetExpClk(UART0_BASE, BOARD_UART_CLK, dev->baud_rate,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN);
    UARTEnableFifo(UART0_BASE);
    UARTEnable(UART0_BASE);
  }
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

//
// Interrupts
//

static uart_callbacks_t _cb;
static uint8_t* _rx_buf;
static uint32_t _rx_size;
static volatile uint32_t _rx_len;
static uint8_t* _tx_buf;
static uint32_t _tx_size;  // power of 2
static volatile uint32_t _tx_head, _tx_tail;

static void _rx_byte()
{
  uint32_t data = HWREG(UART0_BASE + UART_O_DR);
  if ((data & UART_DR_ERRORS) && _cb.error) _cb.error((data & UART_DR_ERRORS) >> 8);
  if (_rx_len < _rx_size) _rx_buf[_rx_len++] = data;
}

static bool _rx_empty() { return HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE; }

static void _tx_fill()
{
  while (_tx_tail != _tx_head && !(HWREG(UART0_BASE + UART_O_FR) & UART_FR_TXFF)) {
    HWREG(UART0_BASE + UART_O_DR) = _tx_buf[_tx_tail];
    _tx_tail = (_tx_tail + 1) & (_tx_size - 1);
  }
  if (_tx_tail == _tx_head) UARTDisableInt(UART0_BASE, UART_INT_TX);
}

static void _uart_isr()
{
  uint32_t status = UARTIntStatus(UART0_BASE, true);
  UARTClearInt(UART0_BASE, status);
  if ((status & UART_ERRORS) && _cb.error) _cb.error((status & UART_ERRORS) >> 7);
  if (_rx_buf && (status & UART_INT_RX))
    for (int i = 0; i < RX_IRQ_BYTES && !_rx_empty(); i++) _rx_byte();
  if (_rx_buf && (status & UART_INT_RT)) {
    while (!_rx_empty()) _rx_byte();
    if (_cb.frame_received) _cb.frame_received();
  }
  if (status & UART_INT_TX) _tx_fill();
}

void uart_enable_irqs(uart_t uart, const uart_callbacks_t* callbacks)
{
  (void)uart;
  _cb = *callbacks;
  UARTSetFifoLevel(UART0_BASE, UART_FIFO_TX2_8, UART_FIFO_RX4_8);
  UARTClearInt(UART0_BASE, 0xFFFFFFFFU);
  UARTEnableInt(UART0_BASE, UART_ERRORS);
  IntRegister(INT_UART0_COMB, _uart_isr);
  IntEnable(INT_UART0_COMB);
}

void uart_enable_rx_irq(uart_t uart, void* buffer, uint32_t size)
{
  (void)uart;
  _rx_buf = buffer;
  _rx_size = size;
  _rx_len = 0;
  UARTEnableInt(UART0_BASE, UART_INT_RX | UART_INT_RT);
}

uint32_t uart_get_rx_len(uart_t uart)
{
  (void)uart;
  return _rx_len;
}

void uart_reset_rx_len(uart_t uart)
{
  (void)uart;
  _rx_len = 0;
}

void uart_enable_tx_irq(uart_t uart, void* buffer, uint32_t size)
{
  (void)uart;
  _tx_buf = buffer;
  _tx_size = size;
  _tx_head = _tx_tail = 0;
}

uint32_t uart_tx_free(uart_t uart)
{
  (void)uart;
  return (_tx_tail - _tx_head - 1) & (_tx_size - 1);
}

// All of it or nothing
bool uart_tx_irq(uart_t uart, const void* data, uint32_t len)
{
  if (len > uart_tx_free(uart)) return false;
  const uint8_t* p = data;
  bool masked = IntDisableMaster();
  while (len--) {
    _tx_buf[_tx_head] = *p++;
    _tx_head = (_tx_head + 1) & (_tx_size - 1);
  }
  _tx_fill();
  if (_tx_tail != _tx_head) UARTEnableInt(UART0_BASE, UART_INT_TX);
  if (!masked) IntEnableMaster();
  return true;
}
