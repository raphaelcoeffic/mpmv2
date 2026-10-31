#pragma once

// The boot loader's chip part (platforms/<family>/bl_port.c), under
// core/bootloader.c: time, the serial ports, flash. The layout (APP_START,
// APP_END, SRAM, FLASH_SECTOR_SIZE) is the platform's bl_layout.h.

#include <stdbool.h>
#include <stdint.h>

// board_init() and a millisecond time
void bl_init(void);
uint32_t bl_millis(void);
// Before the application starts: the ports closed (TX drained), the pins
// back at their board_init() levels, the timer off
void bl_deinit(void);

// Serial ports at 57600 8N1: the console, and the radio's link (one of
// them, or one more). Returns their number (at most BL_PORTS_MAX)
#define BL_PORTS_MAX 2
uint32_t bl_ports_open(void);
bool bl_port_radio(uint32_t port);
// A received byte, with BL_RX_ERROR for framing, parity, break or overrun,
// or -1
#define BL_RX_ERROR 0x100
int bl_port_read(uint32_t port);
void bl_port_write(uint32_t port, uint8_t b);
// The port that sent the sync: the radio's link drives its TX line from
// then on
void bl_port_take(uint32_t port);
// The radio's line at the board's "no radio" level (false without one)
bool bl_radio_absent(void);

// Flash: writes between bl_flash_open() and bl_flash_close() (the flash
// cache off where the chip needs it, invalidated when back on). The data
// must not be in flash
void bl_flash_open(void);
void bl_flash_close(void);
bool bl_flash_erase(uint32_t sector_addr);
bool bl_flash_program(const uint8_t* data, uint32_t addr, uint32_t len);
