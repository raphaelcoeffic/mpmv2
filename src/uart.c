#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <driverlib/prcm.h>
#include <driverlib/debug.h>
#include <driverlib/sys_ctrl.h>

#include <string.h>

#include "uart.h"

#define MAX_UART 2

// FIFO levels:
// - TX: 32 * 1/2 = 16 bytes
// - RX: 32 * 1/4 =  8 bytes
#define FIFO_TX_LEVEL UART_FIFO_TX4_8
#define FIFO_RX_LEVEL UART_FIFO_RX2_8

#define FIFO_RX_SIZE (4 << (FIFO_RX_LEVEL >> 3))

// Error IRQs
#define UART_ERRORS (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)

typedef struct {
  uint8_t *buffer;
  uint32_t size;
  volatile uint32_t rcvd;
} uart_rx_buffer_t;

typedef struct {
  uint8_t *buffer;
  uint32_t size;
  volatile uint32_t head;
  volatile uint32_t tail;
} uart_tx_buffer_t;

typedef struct {
  uart_callbacks_t callbacks;
  uart_rx_buffer_t rx_buf;
  uart_tx_buffer_t tx_buf;
} uart_state_t;

static uart_state_t _uart_state[MAX_UART];

// Lookup tables
static const uint32_t _uart_pwr_domain[MAX_UART] = {
    PRCM_PERIPH_UART0,
    PRCM_PERIPH_UART1,
};

static const uint32_t _uart_base[MAX_UART] = {
    UART0_BASE,
    UART1_BASE,
};

static void _init_pwr_domain(uart_t uart) {
  uint32_t pwr_domain = _uart_pwr_domain[uart];
  PRCMPeripheralRunEnable(pwr_domain);
  PRCMLoadSet();
}

static void _init_state(uart_t uart)
{
  memset(&_uart_state[uart], 0, sizeof(uart_state_t));
}

static void _set_callbacks(uart_t uart, const uart_callbacks_t* callbacks)
{
  memcpy(&_uart_state[uart].callbacks, callbacks, sizeof(uart_callbacks_t));
}

void uart_init(uart_t uart, const uart_device_t* dev)
{
  ASSERT(uart < MAX_UART);

  _init_pwr_domain(uart);
  _init_state(uart);

  uint32_t base = _uart_base[uart];
  IOCPinTypeUart(base, dev->rx, dev->tx, dev->cts, dev->rts);

  UARTDisable(base);

  uint32_t sys_clk = SysCtrlClockGet();
  uint32_t cfg;

  switch (dev->mode) {
  case UART_8N1:
    cfg = UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE;
    break;

  default:
    cfg = 0;
    break;
  }
  UARTConfigSetExpClk(base, sys_clk, dev->baud_rate, cfg);
  UARTFIFOLevelSet(base, FIFO_TX_LEVEL, FIFO_RX_LEVEL);

  uint32_t int_flags =
      UART_ERRORS | UART_INT_RT | UART_INT_RX | UART_INT_TX | UART_INT_CTS;
  UARTIntClear(base, int_flags);
  UARTEnable(base);
}

void uart_print(uart_t uart, const char *str)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];

  while (*str) {
    UARTCharPut(base, *str);
    str++;
  }
}

void uart_put_char(uart_t uart, uint8_t data)
{
  ASSERT(uart < MAX_UART);
  UARTCharPutNonBlocking(_uart_base[uart], data);
}

void uart_put_char_blocking(uart_t uart, uint8_t data)
{
  ASSERT(uart < MAX_UART);
  UARTCharPut(_uart_base[uart], data);
}

uint8_t uart_get_char(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return UARTCharGetNonBlocking(_uart_base[uart]);
}

uint8_t uart_get_char_blocking(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return UARTCharGet(_uart_base[uart]);
}

bool uart_tx_fifo_full(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return !UARTSpaceAvail(_uart_base[uart]);
}

bool uart_rx_fifo_available(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return UARTCharsAvail(_uart_base[uart]);
}

//
// UART IRQ methods
// 

static inline void _rx_irq(uint32_t base, uart_rx_buffer_t* rx, uint32_t len)
{
  while(len--) {
    rx->buffer[rx->rcvd++] = HWREG(base + UART_O_DR);
  }
}

static inline void _rx_flush_fifo(uint32_t base, uart_rx_buffer_t* rx)
{
  while(!(HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE)) {
    rx->buffer[rx->rcvd++] = HWREG(base + UART_O_DR);
  }
}

static inline bool _tx_buffer_write(uart_tx_buffer_t* buf, uint8_t data)
{
  // check if buffer is full
  uint32_t size = buf->size;
  if (((buf->head + 1) & (size - 1)) == buf->tail) {
    return false;
  }

  buf->buffer[buf->head] = data;
  buf->head = (buf->head + 1) & (size - 1);
  return true;
}

static inline bool _tx_buffer_read(uart_tx_buffer_t* buf, uint8_t* data)
{
  // end of buffer
  if (buf->tail == buf->head) return false;

  *data = buf->buffer[buf->tail];
  buf->tail = (buf->tail + 1) & (buf->size - 1);
  return true; 
}

static void _uart_irq(uart_t uart)
{
  uint32_t base = _uart_base[uart];
  uart_state_t* st = &_uart_state[uart];
  uart_callbacks_t* cb = &st->callbacks;

  uint32_t status = UARTIntStatus(base, true);
  UARTIntClear(base, status);

  if (status & UART_ERRORS) {
    uint32_t rx_error = UARTRxErrorGet(base);
    if (cb->error) cb->error((uint8_t)rx_error);
    UARTRxErrorClear(base);
  }

  if ((status & UART_INT_RX) && st->rx_buf.buffer) {
    // we must keep something in the RX FIFO
    // for the timeout IRQ to trigger properly
    _rx_irq(base, &st->rx_buf, FIFO_RX_SIZE - 2);
  }

  if ((status & UART_INT_RT) && st->rx_buf.buffer) {
    _rx_flush_fifo(base, &st->rx_buf);
    if (cb->frame_received) cb->frame_received();
  }

  if (status & UART_INT_TX) {
    while (!(HWREG(base + UART_O_FR) & UART_FR_TXFF)) {
      uint8_t data;
      if (_tx_buffer_read(&st->tx_buf, &data) != 0) {
        HWREG(base + UART_O_DR) = data;
      } else {
        UARTIntDisable(base, UART_INT_TX);
        break;
      }
    }
  }
}

static void _uart0_irq() { _uart_irq(UART0); }
static void _uart1_irq() { _uart_irq(UART1); }

static void (*const _uart_irq_handler[MAX_UART])() = {
    _uart0_irq,
    _uart1_irq,
};

void uart_enable_irqs(uart_t uart, const uart_callbacks_t* callbacks)
{
  ASSERT(uart < MAX_UART);
  _set_callbacks(uart, callbacks);

  uint32_t base = _uart_base[uart];
  UARTIntRegister(base, _uart_irq_handler[uart]);

  uint32_t int_flags = UART_ERRORS;
  UARTIntEnable(base, int_flags);
}

// RX IRQ methods
// 
void uart_enable_rx_irq(uart_t uart, uint8_t* buffer, uint32_t size)
{
  ASSERT(uart < MAX_UART);

  uart_rx_buffer_t* buf = &_uart_state[uart].rx_buf;
  buf->buffer = buffer;
  buf->size = size;
  buf->rcvd = 0;

  uint32_t base = _uart_base[uart];
  UARTIntEnable(base, UART_INT_RT | UART_INT_RX);
}

void uart_disable_rx_irq(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  UARTIntDisable(base, UART_INT_RT | UART_INT_RX);
}

uint32_t uart_get_rx_len(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return _uart_state[uart].rx_buf.rcvd;
}

void uart_reset_rx_len(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  _uart_state[uart].rx_buf.rcvd = 0;
}

// TX IRQ methods
// 
void uart_enable_tx_irq(uart_t uart, uint8_t* buffer, uint32_t size)
{
  ASSERT(uart < MAX_UART);

  uart_tx_buffer_t* buf = &_uart_state[uart].tx_buf;
  buf->head = buf->tail = 0;
  buf->buffer = buffer;
  buf->size = size;
}

void uart_disable_tx_irq(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  UARTIntDisable(base, UART_INT_TX);
}

static inline bool _tx_irq_enabled(uint32_t base)
{
  return HWREG(base + UART_O_IMSC) & UART_INT_TX;
}

static inline bool _tx_buffered(uint32_t uart, uint8_t data)
{
  uint32_t base = _uart_base[uart];
  if (!_tx_irq_enabled(base) && UARTSpaceAvail(base)) {
    UARTCharPutNonBlocking(base, data);
    return true;
  }

  _tx_buffer_write(&_uart_state[uart].tx_buf, data);
  UARTIntEnable(base, UART_INT_TX);

  return true;
}

bool uart_tx_irq(uart_t uart, const uint8_t* data, uint32_t len)
{
  ASSERT(uart < MAX_UART);
  while (len--) {
    if (!_tx_buffered(uart, *data++)) return false;
  }
  return true;
}

bool uart_print_irq(uart_t uart, const char* str)
{
  uint32_t len = strlen(str);
  return uart_tx_irq(uart, (const uint8_t*)str, len);
}

