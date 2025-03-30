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

typedef struct {
  void (*received)(uint8_t byte);
  void (*error)(uart_error_t error);
  uint8_t (*send)(uint8_t* data);
} uart_callbacks_t;

void uart_init(uart_t uart, const uart_device_t* dev);

void uart_enable_irqs(uart_t uart, const uart_callbacks_t* callbacks);

void uart_put_char(uart_t uart, uint8_t data);
uint8_t uart_get_char(uart_t uart);

bool uart_tx_fifo_full(uart_t uart);
bool uart_rx_fifo_available(uart_t uart);

void uart_enable_tx_irq(uart_t uart);
void uart_enable_rx_irq(uart_t uart);

void uart_disable_tx_irq(uart_t uart);
void uart_disable_rx_irq(uart_t uart);

bool uart_tx_irq_enabled(uart_t uart);

void uart_print(uart_t uart, const char *str);
