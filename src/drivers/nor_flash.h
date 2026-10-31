#pragma once

#include "spi.h"

#define NOR_FLASH_PAGE_SIZE   256
#define NOR_FLASH_SECTOR_SIZE 4096

// return != 0 if error, 0 otherwise
int nor_flash_init(spi_t spi, const spi_device_t* dev);

// Called while the flash programs or erases on its own (CS released), and
// after each read: the protocol callbacks keep running during a write. It
// must not use the flash.
void nor_flash_set_yield(void (*yield)());

uint32_t nor_flash_read(uint32_t addr, uint8_t* data, uint32_t len);

// Any length, split at the page boundaries. 0 if OK, -1 if the flash reports
// a program failure
int nor_flash_write(uint32_t addr, const uint8_t* data, uint32_t len);

void nor_flash_sync();

uint32_t nor_flash_size();

// 4KB erase. 0 if OK, -1 if misaligned or the flash reports an erase failure
int nor_flash_erase(uint32_t address);

// 32KB erase
int nor_flash_erase_block(uint32_t address);

void nor_flash_erase_all();
