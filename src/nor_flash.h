#pragma once

#include "spi.h"

// return != 0 if error, 0 otherwise
int nor_flash_init(spi_t spi, const spi_device_t* dev);

uint32_t nor_flash_read(uint32_t addr, uint8_t* data, uint32_t len);
uint32_t nor_flash_write(uint32_t addr, const uint8_t* data, uint32_t len);

void nor_flash_sync();

uint32_t nor_flash_size();

// 4KB erase
int nor_flash_erase(uint32_t address);

// 32KB erase
int nor_flash_erase_block(uint32_t address);

void nor_flash_erase_all();
