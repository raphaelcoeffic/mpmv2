#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/prcm.h>
#include <driverlib/ssi.h>
#include <driverlib/sys_ctrl.h>

#include "spi.h"

#define MAX_SPI 2

// Lookup tables
static const uint32_t _spi_pwr_domain[MAX_SPI] = {
  PRCM_PERIPH_SSI0,
  PRCM_PERIPH_SSI1,
};

static const uint32_t _spi_base[MAX_SPI] = {
  SSI0_BASE,
  SSI1_BASE,
};

static const uint32_t _spi_frame_format[] = {
  SSI_FRF_MOTO_MODE_0, // SPI_POL0_PHA0
  SSI_FRF_MOTO_MODE_1, // SPI_POL0_PHA1
  SSI_FRF_MOTO_MODE_2, // SPI_POL1_PHA0
  SSI_FRF_MOTO_MODE_3, // SPI_POL1_PHA1
};

#define MAX_FRAME_FORMATS (sizeof(_spi_frame_format) / sizeof(uint32_t))

static uint32_t _spi_cs[MAX_SPI];

static void _init_pwr_domain(spi_t spi) {
  uint32_t pwr_domain = _spi_pwr_domain[spi];
  PRCMPeripheralRunEnable(pwr_domain);
  PRCMLoadSet();
}

void spi_init(spi_t spi, const spi_device_t* dev)
{
  ASSERT(spi < MAX_SPI);
  ASSERT(dev && (dev->frame_format < MAX_FRAME_FORMATS));

  _init_pwr_domain(spi);

  uint32_t base = _spi_base[spi];
  IOCPinTypeSsiMaster(base, dev->rx, dev->tx, IOID_UNUSED, dev->clk);

  uint32_t cs = dev->cs;
  _spi_cs[spi] = cs;

  if (cs != IOID_UNUSED) {
    GPIO_setDio(cs);
    IOCPinTypeGpioOutput(cs);
  }

  SSIDisable(base);

  uint32_t sys_clk = SysCtrlClockGet();
  uint32_t frame_format = _spi_frame_format[dev->frame_format];
  SSIConfigSetExpClk(base, sys_clk, frame_format, SSI_MODE_MASTER,
                     dev->bit_rate, dev->data_width);

  SSIEnable(base);
}

void spi_select(spi_t spi)
{
  ASSERT(spi < MAX_SPI);
  GPIO_clearDio(_spi_cs[spi]);
}

void spi_unselect(spi_t spi)
{
  ASSERT(spi < MAX_SPI);
  GPIO_setDio(_spi_cs[spi]);
}

#define SPI_TRANSFER_LOOP(base, tx, tx_inc, rx, rx_inc, len)                   \
  uint32_t tx_count = len;                                                     \
  while (len--) {                                                              \
    bool put = true;                                                           \
    while (tx_count && put) {                                                  \
      put = HWREG(base + SSI_O_SR) & SSI_TX_NOT_FULL;                          \
      if (put) {                                                               \
        HWREG(base + SSI_O_DR) = *tx;                                          \
        tx += tx_inc;                                                          \
        tx_count--;                                                            \
      }                                                                        \
    }                                                                          \
    while (!(HWREG(base + SSI_O_SR) & SSI_RX_NOT_EMPTY)) {                     \
    }                                                                          \
    *rx = HWREG(base + SSI_O_DR);                                              \
    rx += rx_inc;                                                              \
  }

static uint16_t _scratch;

void spi_transfer_8(spi_t spi, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  ASSERT(spi < MAX_SPI);
  uint32_t base = _spi_base[spi];
  uint32_t tx_inc = 1, rx_inc = 1;
  if (!tx) { tx = (uint8_t*)&_scratch; tx_inc = 0; }
  if (!rx) { rx = (uint8_t*)&_scratch; rx_inc = 0; }
  SPI_TRANSFER_LOOP(base, tx, tx_inc, rx, rx_inc, len);
}

void spi_transfer_16(spi_t spi, const uint16_t *tx, uint16_t *rx, uint32_t len)
{
  ASSERT(spi < MAX_SPI);
  uint32_t base = _spi_base[spi];
  uint32_t tx_inc = 1, rx_inc = 1;
  if (!tx) { tx = &_scratch; tx_inc = 0; }
  if (!rx) { rx = &_scratch; rx_inc = 0; }
  SPI_TRANSFER_LOOP(base, tx, tx_inc, rx, rx_inc, len);
}

void spi_read_8(spi_t spi, uint8_t* data, uint32_t len)
{
  spi_transfer_8(spi, 0, data, len);
}

void spi_read_16(spi_t spi, uint16_t* data, uint32_t len)
{
  spi_transfer_16(spi, 0, data, len);
}

void spi_write_8(spi_t spi, const uint8_t* data, uint32_t len)
{
  spi_transfer_8(spi, data, 0, len);
}

void spi_write_16(spi_t spi, const uint16_t* data, uint32_t len)
{
  spi_transfer_16(spi, data, 0, len);
}
