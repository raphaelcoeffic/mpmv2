#include "board.h"
#include "dma.h"
#include "serial.h"
#include "uart.h"

#include <string.h>

// Frame received flag
volatile bool frame_received = false;
volatile uint32_t rx_len = 0;

// IRQ RX buffer
uint8_t rx_buf[RX_BUFFER_SIZE];

// IRQ TX buffer
static uint8_t tx_buf[TX_BUFFER_SIZE];

static void _serial_rx_timeout()
{
  rx_len = uart_get_rx_len(UART0);
  frame_received = true;
}

void serial_init()
{
  uart_device_t uart0 = {
    .mode = UART_8N1,
    .baud_rate = SERIAL_BAUDRATE,
    .rx = SERIAL_RX_IOD,
    .tx = SERIAL_TX_IOD,
    .cts = IOID_UNUSED,
    .rts = IOID_UNUSED,
  };
  uart_init(UART0, &uart0);

  // UART IRQ callbacks
  uart_callbacks_t uart0_cb = {
    .frame_received = _serial_rx_timeout,
    .error = 0, // TODO
  };
  uart_enable_irqs(UART0, &uart0_cb);
  uart_enable_rx_irq(UART0, rx_buf, RX_BUFFER_SIZE);
  uart_enable_tx_irq(UART0, tx_buf, TX_BUFFER_SIZE);
  dma_init();
}

void serial_print(const char* str)
{
  uart_print_irq(UART0, str);
}

void serial_write(const uint8_t* data, uint32_t len)
{
  uart_tx_irq(UART0, data, len);
}

static inline void serial_wait_dma() { uart_tx_dma_wait(UART0); }

void serial_write_dma(const void* data, uint32_t len, bool blocking)
{
  serial_wait_dma();
  uart_tx_dma(UART0, data, len);
  if (blocking) serial_wait_dma();
}

void serial_print_dma(const char* str)
{
  uint32_t len = strlen(str);
  serial_write_dma(str, len, true);
}

