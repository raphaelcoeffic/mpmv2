// spi.h on the CC27xx (platforms/cc26xx/spi.c on the CC2652): SPI0 / SPI1
// as controller, polled, CS a GPIO driven by spi_select(). The "DMA" calls
// are polled too (the NOR flash's 256 byte pages: some 200 us at 12 MHz).
// Clock: the 48 MHz peripheral clock (CLKSVT / 2), as UART0. The pins'
// functions come from their DIO (pin mux table, SWRS306 RHA package)

#include "spi.h"

#include <driverlib/clkctl.h>
#include <driverlib/gpio.h>
#include <driverlib/spi.h>
#include <inc/hw_spi.h>

#include "board.h"

#define SPI_CLK 48000000

typedef struct {
  uint8_t dio;
  uint8_t func;  // IOC PORTCFG
} spi_pin_t;

typedef struct {
  uint32_t base;
  uint32_t clk;
  spi_pin_t pico[3];  // func 0: none
  spi_pin_t poci[3];
  spi_pin_t sclk[3];
} spi_lut_t;

static const spi_lut_t _spi_lut[] = {
    {SPI0_BASE, CLKCTL_SPI0,
     {{4, 1}, {5, 2}, {21, 1}},
     {{4, 2}, {5, 1}, {22, 1}},
     {{3, 1}, {20, 1}}},
    {SPI1_BASE, CLKCTL_SPI1,
     {{11, 2}, {12, 1}, {27, 1}},
     {{11, 1}, {12, 2}, {28, 1}},
     {{8, 1}, {15, 1}, {24, 1}}},
};

static uint32_t _spi_cs[2] = {IOID_UNUSED, IOID_UNUSED};

static void _pin(const spi_pin_t* pins, uint32_t dio, uint32_t cfg)
{
  if (dio == IOID_UNUSED) return;
  for (int i = 0; i < 3; i++) {
    if (pins[i].func && pins[i].dio == dio) {
      board_ioc(dio, pins[i].func | cfg);
      return;
    }
  }
}

void spi_init(spi_t spi, const spi_device_t* dev)
{
  const spi_lut_t* lut = &_spi_lut[spi];
  CLKCTLEnable(CLKCTL_BASE, lut->clk);

  _spi_cs[spi] = dev->cs;
  if (dev->cs != IOID_UNUSED) {
    GPIOSetDio(dev->cs);
    board_ioc(dev->cs, IOC_IOC0_PORTCFG_BASE);
    GPIOSetOutputEnableDio(dev->cs, GPIO_OUTPUT_ENABLE);
  }
  _pin(lut->pico, dev->tx, 0);
  _pin(lut->poci, dev->rx, IOC_IOC0_INPEN_EN);
  _pin(lut->sclk, dev->clk, 0);

  static const uint32_t frf[] = {SPI_FRF_MOTO_MODE_0, SPI_FRF_MOTO_MODE_1,
                                 SPI_FRF_MOTO_MODE_2, SPI_FRF_MOTO_MODE_3};
  SPIDisable(lut->base);
  SPIConfigSetExpClk(lut->base, SPI_CLK, frf[dev->frame_format], SPI_MODE_CONTROLLER,
                     dev->bit_rate, dev->data_width);
  SPIEnable(lut->base);
}

void spi_select(spi_t spi) { GPIOClearDio(_spi_cs[spi]); }
void spi_unselect(spi_t spi) { GPIOSetDio(_spi_cs[spi]); }

// One word out, one in, the TX FIFO kept a few words ahead
static void _transfer(spi_t spi, const void* tx, void* rx, uint32_t len, int size)
{
  uint32_t base = _spi_lut[spi].base;
  uint32_t sent = 0, got = 0, dummy;

  while (SPIGetDataNonBlocking(base, &dummy)) {}
  while (got < len) {
    if (sent < len && sent - got < 4 && (HWREG(base + SPI_O_STA) & SPI_STA_TNF)) {
      uint32_t w = 0xFFFF;
      if (tx) w = size == 1 ? ((const uint8_t*)tx)[sent] : ((const uint16_t*)tx)[sent];
      HWREG(base + SPI_O_TXDATA) = w;
      sent++;
    }
    if (!(HWREG(base + SPI_O_STA) & SPI_STA_RFE)) {
      uint32_t w = HWREG(base + SPI_O_RXDATA);
      if (rx) {
        if (size == 1) ((uint8_t*)rx)[got] = w;
        else ((uint16_t*)rx)[got] = w;
      }
      got++;
    }
  }
}

void spi_transfer_8(spi_t spi, const uint8_t* tx, uint8_t* rx, uint32_t len) { _transfer(spi, tx, rx, len, 1); }
void spi_transfer_16(spi_t spi, const uint16_t* tx, uint16_t* rx, uint32_t len) { _transfer(spi, tx, rx, len, 2); }
void spi_read_8(spi_t spi, uint8_t* data, uint32_t len) { _transfer(spi, 0, data, len, 1); }
void spi_read_16(spi_t spi, uint16_t* data, uint32_t len) { _transfer(spi, 0, data, len, 2); }
void spi_write_8(spi_t spi, const uint8_t* data, uint32_t len) { _transfer(spi, data, 0, len, 1); }
void spi_write_16(spi_t spi, const uint16_t* data, uint32_t len) { _transfer(spi, data, 0, len, 2); }

void spi_read_dma_8(spi_t spi, uint8_t* data, uint32_t len, bool blocking) { (void)blocking; spi_read_8(spi, data, len); }
void spi_read_dma_16(spi_t spi, uint16_t* data, uint32_t len, bool blocking) { (void)blocking; spi_read_16(spi, data, len); }
void spi_write_dma_8(spi_t spi, const uint8_t* data, uint32_t len, bool blocking) { (void)blocking; spi_write_8(spi, data, len); }
void spi_write_dma_16(spi_t spi, const uint16_t* data, uint32_t len, bool blocking) { (void)blocking; spi_write_16(spi, data, len); }
void spi_wait_dma_done(spi_t spi) { (void)spi; }
