#include "nor_flash.h"
#include "board.h"

static const spi_device_t _flash_dev = {
  .frame_format = SPI_POL0_PHA0,
  .data_width = 8,
  .bit_rate = 12000000, // 12 MHz
  .rx = FLASH_MISO,
  .tx = FLASH_MOSI,
  .clk = FLASH_SCLK,
  .cs = FLASH_CS,
};

static bool flash_initialised = false;

int flash_init()
{
  if (flash_initialised) {
    return 0;
  }
  
  if (nor_flash_init(FLASH_SPI, &_flash_dev) != 0) {
    return -1;
  }

  flash_initialised = true;
  return 0;
}
