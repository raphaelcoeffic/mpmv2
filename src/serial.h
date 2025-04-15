#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SERIAL_BAUDRATE 921600

#define RX_BUFFER_SIZE 128
#define TX_BUFFER_SIZE 128 // must be a power of 2

extern uint8_t rx_buf[RX_BUFFER_SIZE];

// Frame received flag
extern volatile bool frame_received;
extern volatile uint32_t rx_len;

void serial_init();

void serial_print(const char* str);
void serial_write(const uint8_t* data, uint32_t len);

void serial_write_dma(const void* data, uint32_t len, bool blocking);
void serial_print_dma(const char* str);
