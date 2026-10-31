#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <driverlib/prcm.h>
#include <driverlib/debug.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/udma.h>

#include <string.h>

#include "uart.h"

// CC26x0: one UART, no TX DMA (its 1 KB control table: 20 KB of RAM)
#if defined(MCU_CC26X0)
#define MAX_UART 1
#else
#define UART_DMA
#define MAX_UART 2
#include "dma.h"
#endif

// FIFO levels:
// - TX: 32 * 1/2 = 16 bytes
// - RX: 32 * 1/4 =  8 bytes
#define FIFO_TX_LEVEL UART_FIFO_TX4_8
#define FIFO_RX_LEVEL UART_FIFO_RX2_8

#define FIFO_RX_SIZE (4 << (FIFO_RX_LEVEL >> 3))

// Error IRQs
#define UART_ERRORS (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)

#define MAX_DMA_XFER_SIZE(size)  \
  ((size) > UDMA_XFER_SIZE_MAX ? UDMA_XFER_SIZE_MAX : (size))

#define DMA_CHANNEL_NUM(mask) ((uint32_t)__builtin_ctz(mask))

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
  uint32_t tx_dma_channel;
} uart_state_t;

static uart_state_t _uart_state[MAX_UART];

// Lookup tables
static const uint32_t _uart_pwr_domain[MAX_UART] = {
    PRCM_PERIPH_UART0,
#if MAX_UART > 1
    PRCM_PERIPH_UART1,
#endif
};

static const uint32_t _uart_base[MAX_UART] = {
    UART0_BASE,
#if MAX_UART > 1
    UART1_BASE,
#endif
};

// IOC ports: RX, TX, CTS, RTS (IOCPinTypeUart() only knows UART0)
static const uint32_t _uart_ioc_port[MAX_UART][4] = {
    {IOC_PORT_MCU_UART0_RX, IOC_PORT_MCU_UART0_TX, IOC_PORT_MCU_UART0_CTS, IOC_PORT_MCU_UART0_RTS},
#if MAX_UART > 1
    {IOC_PORT_MCU_UART1_RX, IOC_PORT_MCU_UART1_TX, IOC_PORT_MCU_UART1_CTS, IOC_PORT_MCU_UART1_RTS},
#endif
};

#if defined(UART_DMA)
static const uint8_t _uart_tx_dma_channel[MAX_UART] = {
  UDMA_CHAN_UART0_TX,
  UDMA_CHAN_UART1_TX,
};
#endif

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
#if defined(UART_DMA)
  dma_init();
#endif

  uint32_t base = _uart_base[uart];
  const uint32_t pins[4] = {dev->rx, dev->tx, dev->cts, dev->rts};
  for (int i = 0; i < 4; i++) {
    if (pins[i] != IOID_UNUSED)
      IOCPortConfigureSet(pins[i], _uart_ioc_port[uart][i], i & 1 ? IOC_STD_OUTPUT : IOC_STD_INPUT);
  }

  UARTDisable(base);

  uint32_t sys_clk = SysCtrlClockGet();
  uint32_t cfg;

  switch (dev->mode) {
  case UART_8N1:
    cfg = UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE;
    break;

  case UART_8E2:
    cfg = UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN;
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
    uint8_t data = HWREG(base + UART_O_DR);
    if (rx->rcvd < rx->size) rx->buffer[rx->rcvd++] = data;
    // TODO: else set overflow flag?
  }
}

static inline void _rx_flush_fifo(uint32_t base, uart_rx_buffer_t* rx)
{
  while(!(HWREG(base + UART_O_FR) & UART_FR_RXFE)) {
    uint8_t data = HWREG(base + UART_O_DR);
    if (rx->rcvd < rx->size) rx->buffer[rx->rcvd++] = data;
    // TODO: else set overflow flag?
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

#if defined(UART_DMA)
static void _dma_done_irq(uart_t uart);
#endif

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

#if defined(UART_DMA)
  if (uDMAIntStatus(UDMA0_BASE) & st->tx_dma_channel) {
    _dma_done_irq(uart);
  }

  if (status & UART_INT_EOT) {
    UARTIntDisable(base, UART_INT_EOT);
    st->tx_dma_channel = 0;
  }
#endif
}

static void _uart0_irq() { _uart_irq(UART0); }
#if MAX_UART > 1
static void _uart1_irq() { _uart_irq(UART1); }
#endif

static void (*const _uart_irq_handler[MAX_UART])() = {
    _uart0_irq,
#if MAX_UART > 1
    _uart1_irq,
#endif
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
void uart_enable_rx_irq(uart_t uart, void* buffer, uint32_t size)
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
void uart_enable_tx_irq(uart_t uart, void* buffer, uint32_t size)
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

uint32_t uart_tx_free(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uart_tx_buffer_t* buf = &_uart_state[uart].tx_buf;
  return (buf->tail - buf->head - 1) & (buf->size - 1);
}

bool uart_tx_irq(uart_t uart, const void* data, uint32_t len)
{
  ASSERT(uart < MAX_UART);
  const uint8_t* tx = (const uint8_t*)data;
  while (len--) {
    if (!_tx_buffered(uart, *tx++)) return false;
  }
  return true;
}

bool uart_print_irq(uart_t uart, const char* str)
{
  uint32_t len = strlen(str);
  return uart_tx_irq(uart, (const uint8_t*)str, len);
}

#if defined(UART_DMA)
// TX DMA methods
//

static void _start_tx_dma_xfer(uint32_t uart, uint32_t dma_channel,
                               void *buffer, uint32_t len) {

  uint32_t ctrl = UDMA_SIZE_8 | UDMA_SRC_INC_8 | UDMA_DST_INC_NONE | UDMA_ARB_4;
  uDMAChannelControlSet(UDMA0_BASE, dma_channel, ctrl);

  uint32_t mode = UDMA_MODE_BASIC;
  uint32_t base = _uart_base[uart];

  uDMAChannelTransferSet(UDMA0_BASE, dma_channel, mode, (void *)buffer,
                         (void *)(base + UART_O_DR), len);

  _uart_state[uart].tx_dma_channel |= (1 << dma_channel);
  uDMAChannelEnable(UDMA0_BASE, dma_channel);
  UARTDMAEnable(base, UART_DMA_TX);
}

void uart_tx_dma(uart_t uart, const void* buffer, uint32_t len)
{
  ASSERT(uart < MAX_UART);

  // disable RX & TX channels
  uint8_t dma_channel = _uart_tx_dma_channel[uart];
  uDMAChannelDisable(UDMA0_BASE, dma_channel);

  // set buffer
  uart_tx_buffer_t* buf = &_uart_state[uart].tx_buf;
  buf->buffer = (uint8_t*)buffer;
  buf->size = len;

  len = MAX_DMA_XFER_SIZE(len);
  buf->head = len;
  buf->tail = 0;

  _start_tx_dma_xfer(uart, dma_channel, (void *)buffer, len);
}

bool uart_tx_dma_done(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  return _uart_state[uart].tx_dma_channel == 0;
}

void uart_tx_dma_wait(uart_t uart)
{
  ASSERT(uart < MAX_UART);
  uint32_t base = _uart_base[uart];
  uart_state_t* st = &_uart_state[uart];
  while (st->tx_dma_channel != 0) {}
  while (HWREG(base + UART_O_FR) & UART_FR_BUSY) {}
}

static void _dma_done_irq(uart_t uart)
{
  // disable DMA channel & TX DMA
  uint32_t base = _uart_base[uart];
  uart_state_t* st = &_uart_state[uart];
  uint32_t dma_channel_mask = st->tx_dma_channel;
  uint32_t dma_channel = DMA_CHANNEL_NUM(dma_channel_mask);

  uDMAChannelDisable(UDMA0_BASE, dma_channel);
  UARTDMADisable(base, UART_DMA_TX);
  uDMAIntClear(UDMA0_BASE, dma_channel_mask);

  // update buffer state
  uart_tx_buffer_t* buf = &st->tx_buf;
  buf->tail += (buf->head - buf->tail);

  if (buf->tail < buf->size) {
    // transfer not yet complete
    uint32_t xfer_len = MAX_DMA_XFER_SIZE(buf->size - buf->tail);
    buf->head = buf->tail + xfer_len;

    const uint8_t* buffer = buf->buffer + buf->tail;
    _start_tx_dma_xfer(uart, dma_channel, (void *)buffer, xfer_len);
  } else {
    // transfer complete
    UARTIntEnable(base, UART_INT_EOT);
  }
}
#endif
