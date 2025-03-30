#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <driverlib/prcm.h>
#include <driverlib/debug.h>
#include <driverlib/sys_ctrl.h>

#include <string.h>

#include "uart.h"

#define MAX_UART 2

// Lookup tables
static const uint32_t _uart_pwr_domain[MAX_UART] = {
  PRCM_PERIPH_UART0,
  PRCM_PERIPH_UART1,
};

static const uint32_t _uart_base[MAX_UART] = {
  UART0_BASE,
  UART1_BASE,
};

static void _uart0_irq();
static void _uart1_irq();

static void (* const _uart_irq_handler[MAX_UART])() = {
  _uart0_irq,
  _uart1_irq,
};

static uart_callbacks_t _uart_callbacks[MAX_UART];

static inline void _uart_irq(uart_t uart)
{
  uint32_t base = _uart_base[uart];
  uart_callbacks_t* cb = &_uart_callbacks[uart];

  uint32_t status = UARTIntStatus(base, true);
  UARTIntClear(base, status);

  if (status & (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)) {
    uint32_t rx_error = UARTRxErrorGet(base);
    if (cb->error) cb->error((uint8_t)rx_error);
    UARTRxErrorClear(base);
  }

  if ((status & UART_INT_RX) && cb->received) {
    while (!(HWREG(base + UART_O_FR) & UART_FR_RXFE)) {
      uint32_t data = HWREG(base + UART_O_DR);
      cb->received((uint8_t)data);
    }
  }

  if ((status & UART_INT_TX) && cb->send) {
    while (!(HWREG(base + UART_O_FR) & UART_FR_TXFF)) {
      uint8_t data;
      if (cb->send(&data) != 0) {
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

static void _init_pwr_domain(uart_t uart) {
  uint32_t pwr_domain = _uart_pwr_domain[uart];
  PRCMPeripheralRunEnable(pwr_domain);
  PRCMLoadSet();
}

static void _init_callbacks(uart_t uart)
{
  memset(&_uart_callbacks[uart], 0, sizeof(uart_callbacks_t));
}

static void _set_callbacks(uart_t uart, const uart_callbacks_t* callbacks)
{
  memcpy(&_uart_callbacks[uart], callbacks, sizeof(uart_callbacks_t));
}

void uart_init(uart_t uart, const uart_device_t* dev)
{
  ASSERT(uart < MAX_UART);

  _init_pwr_domain(uart);
  _init_callbacks(uart);

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

	UARTIntClear(base,
		UART_INT_OE | UART_INT_BE | UART_INT_PE |
		UART_INT_FE | UART_INT_RT | UART_INT_TX |
		UART_INT_RX | UART_INT_CTS);

  UARTEnable(base);

  // TODO: setting to allow disabling the FIFO
  // UARTFIFODisable(base);
}

void uart_enable_irqs(uart_t uart, const uart_callbacks_t* callbacks)
{
  ASSERT(uart < MAX_UART);

  uint32_t base = _uart_base[uart];
  uint32_t int_flags = UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE |
                       UART_INT_RX;

  _set_callbacks(uart, callbacks);

  UARTIntRegister(base, _uart_irq_handler[uart]);
  UARTIntEnable(base, int_flags);
}

bool uart_tx_fifo_full(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return !UARTSpaceAvail(_uart_base[uart]);
}

void uart_put_char(uart_t uart, uint8_t data)
{
  ASSERT(uart < MAX_UART);
  UARTCharPutNonBlocking(_uart_base[uart], data);
}

void uart_enable_tx_irq(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  UARTIntEnable(base, UART_INT_TX);
}

void uart_disable_tx_irq(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  UARTIntDisable(base, UART_INT_TX);
}

bool uart_tx_irq_enabled(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  return HWREG(base + UART_O_IMSC) & UART_INT_TX; 
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

