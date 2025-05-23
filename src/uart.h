#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  UART0,
  UART1,
} uart_t;

typedef enum {
  UART_8N1,
} uart_mode_t;

typedef enum {
  UART_ERROR_OVERRUN = 8,
  UART_ERROR_BREAK = 4,
  UART_ERROR_PARITY = 2,
  UART_ERROR_FRAMING = 1,
} uart_error_t;

typedef struct {
  uart_mode_t mode;
  uint32_t baud_rate;
  uint32_t rx;
  uint32_t tx;
  uint32_t cts;
  uint32_t rts;
} uart_device_t;

// Initialise UART without any IRQ or DMA
void uart_init(uart_t uart, const uart_device_t* dev);

// Blocking UART write
void uart_print(uart_t uart, const char *str);

// Write into UART internal FIFO (non-blocking)
void uart_put_char(uart_t uart, uint8_t data);

// Write into UART internal FIFO
void uart_put_char_blocking(uart_t uart, uint8_t data);

// Read from UART internal FIFO (non-blocking)
uint8_t uart_get_char(uart_t uart);

// Read from UART internal FIFO
uint8_t uart_get_char_blocking(uart_t uart);

// UART internal FIFO status
bool uart_tx_fifo_full(uart_t uart);
bool uart_rx_fifo_available(uart_t uart);

//
// UART IRQ methods
// 

typedef struct {
  void (*frame_received)();
  void (*error)(uart_error_t error);
} uart_callbacks_t;

// Initialise IRQ mode
void uart_enable_irqs(uart_t uart, const uart_callbacks_t* callbacks);

// RX IRQ methods
void uart_enable_rx_irq(uart_t uart, void* buffer, uint32_t size);
void uart_disable_rx_irq(uart_t uart);

void uart_reset_rx_len(uart_t uart);
uint32_t uart_get_rx_len(uart_t uart);

// TX IRQ methods
void uart_enable_tx_irq(uart_t uart, void* buffer, uint32_t size);
void uart_disable_tx_irq(uart_t uart);
bool uart_tx_irq_enabled(uart_t uart);

bool uart_tx_irq(uart_t uart, const void* data, uint32_t len);
bool uart_print_irq(uart_t uart, const char* str);

// TX DMA methods
void uart_tx_dma(uart_t uart, const void* buffer, uint32_t len);
bool uart_tx_dma_done(uart_t uart);
void uart_tx_dma_wait(uart_t uart);
