#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  SPI0,
  SPI1,
} spi_t;

typedef enum {
  SPI_POL0_PHA0,
  SPI_POL0_PHA1,
  SPI_POL1_PHA0,
  SPI_POL1_PHA1,
} spi_frame_format_t;

typedef struct {
  spi_frame_format_t frame_format;
  uint32_t data_width;
  uint32_t bit_rate;
  uint32_t rx;
  uint32_t tx;
  uint32_t clk;
  uint32_t cs;
} spi_device_t;

void spi_init(spi_t spi, const spi_device_t* dev);

void spi_select(spi_t spi);
void spi_unselect(spi_t spi);

void spi_transfer_8(spi_t spi, const uint8_t *tx, uint8_t *rx, uint32_t len);
void spi_transfer_16(spi_t spi, const uint16_t *tx, uint16_t *rx, uint32_t len);

void spi_read_8(spi_t spi, uint8_t* data, uint32_t len);
void spi_read_16(spi_t spi, uint16_t* data, uint32_t len);

void spi_write_8(spi_t spi, const uint8_t* data, uint32_t len);
void spi_write_16(spi_t spi, const uint16_t* data, uint32_t len);
