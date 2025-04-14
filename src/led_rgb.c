#include <driverlib/ioc.h>
#include "spi.h"

// Get SPI value corresponding to a bit at index n in a grb color on 24 bits
//  1 bit is 0b110
//  0 bit is 0b100
//
#define _TO_SPI_BITS(val, bitPos) \
  (((1 << bitPos & val) ? 0x06 : 0x04) << (bitPos * 3))

#define _TO_SPI_WORD(val) \
  (_TO_SPI_BITS(val, 0) | _TO_SPI_BITS(val, 1) | _TO_SPI_BITS(val, 2) | _TO_SPI_BITS(val, 3))

static const uint16_t _to_spi[] = {
    _TO_SPI_WORD(0x0), _TO_SPI_WORD(0x1), _TO_SPI_WORD(0x2), _TO_SPI_WORD(0x3),
    _TO_SPI_WORD(0x4), _TO_SPI_WORD(0x5), _TO_SPI_WORD(0x6), _TO_SPI_WORD(0x7),
    _TO_SPI_WORD(0x8), _TO_SPI_WORD(0x9), _TO_SPI_WORD(0xA), _TO_SPI_WORD(0xB),
    _TO_SPI_WORD(0xC), _TO_SPI_WORD(0xD), _TO_SPI_WORD(0xE), _TO_SPI_WORD(0xF),
};

static spi_t _led_spi;

void led_rgb_init(spi_t spi, uint32_t dio)
{
  const spi_device_t dev = {
    .frame_format = SPI_POL0_PHA1,
    .data_width = 12,
    .bit_rate = 2400000,
    .rx = IOID_UNUSED,
    .tx = dio,
    .clk = IOID_UNUSED,
    .cs = IOID_UNUSED,
  };

  spi_init(spi, &dev);
  _led_spi = spi;
}

void led_rgb_set_color(uint32_t color)
{
  // GRB pixel format
  // Total runtime: 9.32 us

  // Compute SPI data: 2.9 us
  uint16_t pixel_data[6];
  pixel_data[0] = _to_spi[(color >> 4) & 0xF];
  pixel_data[1] = _to_spi[color & 0xF];
  color >>= 8;

  pixel_data[2] = _to_spi[(color >> 4) & 0xF];
  pixel_data[3] = _to_spi[color & 0xF];
  color >>= 8;
  
  pixel_data[4] = _to_spi[(color >> 4) & 0xF];
  pixel_data[5] = _to_spi[color & 0xF];

  // Send data: 6.42 us
  spi_write_16(_led_spi, pixel_data, 6);
}
