#include <driverlib/ioc.h>
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
  IOCPinTypeSsiMaster(base, dev->rx, dev->tx, dev->fss, dev->clk);

  SSIDisable(base);

  uint32_t sys_clk = SysCtrlClockGet();
  uint32_t frame_format = _spi_frame_format[dev->frame_format];
  SSIConfigSetExpClk(base, sys_clk, frame_format, SSI_MODE_MASTER,
                     dev->bit_rate, dev->data_width);

  SSIEnable(base);
}

void spi_write_16(spi_t spi, uint16_t* data, uint32_t len)
{
  ASSERT(spi < MAX_SPI);
  uint32_t base = _spi_base[spi];
  while(len--) {
    SSIDataPut(base, *data++);
  }
}
