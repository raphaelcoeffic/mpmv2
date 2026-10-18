#pragma once

#include "uart.h"

#include <stdint.h>
#include <stdbool.h>

// #define SERIAL_BAUDRATE 921600
#define SERIAL_BAUDRATE 100000

#define RX_BUFFER_SIZE 128
#define TX_BUFFER_SIZE 128 // must be a power of 2

extern uint8_t rx_buf[RX_BUFFER_SIZE];

// Frame received flag
extern volatile bool serial_frame_received;
extern volatile uint32_t rx_len;

void serial_init(uint32_t baudrate, uart_mode_t mode);

// Stop receiving (the RX pin can then be used for signal detection)
void serial_stop();

void serial_print(const char* str);
void serial_write(const uint8_t* data, uint32_t len);

void serial_write_dma(const void* data, uint32_t len, bool blocking);
void serial_print_dma(const char* str);
