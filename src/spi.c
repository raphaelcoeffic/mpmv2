#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/prcm.h>
#include <driverlib/ssi.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/udma.h>

#include "spi.h"
#include "dma.h"

#define MAX_SPI 2

typedef struct {
  uint32_t channel;
  uint32_t mask;
} dma_lut_t;

typedef struct {
  uint32_t base;
  dma_lut_t rx_dma;
  dma_lut_t tx_dma;
} spi_lut_t;

// Lookup tables
static const uint32_t _spi_pwr_domain[MAX_SPI] = {
  PRCM_PERIPH_SSI0,
  PRCM_PERIPH_SSI1,
};

static const spi_lut_t _spi_lut[MAX_SPI] = {
    {SSI0_BASE,
     {UDMA_CHAN_SSI0_RX, (1 << UDMA_CHAN_SSI0_RX)},
     {UDMA_CHAN_SSI0_TX, (1 << UDMA_CHAN_SSI0_TX)}},
    {SSI1_BASE,
     {UDMA_CHAN_SSI1_RX, (1 << UDMA_CHAN_SSI1_RX)},
     {UDMA_CHAN_SSI1_TX, (1 << UDMA_CHAN_SSI1_TX)}},
};

static const uint32_t _spi_frame_format[] = {
    SSI_FRF_MOTO_MODE_0, // SPI_POL0_PHA0
    SSI_FRF_MOTO_MODE_1, // SPI_POL0_PHA1
    SSI_FRF_MOTO_MODE_2, // SPI_POL1_PHA0
    SSI_FRF_MOTO_MODE_3, // SPI_POL1_PHA1
};

#define MAX_FRAME_FORMATS (sizeof(_spi_frame_format) / sizeof(uint32_t))

static uint32_t _spi_cs[MAX_SPI];
static volatile bool _spi_dma_active[MAX_SPI];

static void _init_pwr_domain(spi_t spi) {
  uint32_t pwr_domain = _spi_pwr_domain[spi];
  PRCMPeripheralRunEnable(pwr_domain);
  PRCMLoadSet();
}

static void _spi_irq(spi_t spi);
static void _spi0_irq() { _spi_irq(SPI0); }
static void _spi1_irq() { _spi_irq(SPI1); }

static void (*const _spi_irq_handler[MAX_SPI])() = {
    _spi0_irq,
    _spi1_irq,
};

void spi_init(spi_t spi, const spi_device_t* dev)
{
  ASSERT(spi < MAX_SPI);
  ASSERT(dev && (dev->frame_format < MAX_FRAME_FORMATS));

  _init_pwr_domain(spi);
  dma_init();

  uint32_t base = _spi_lut[spi].base;
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

  SSIIntRegister(base, _spi_irq_handler[spi]);
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
  {                                                                            \
    while (HWREG(base + SSI_O_SR) & SSI_SR_BSY) {                              \
    }                                                                          \
    uint32_t tx_count = len;                                                   \
    while (len--) {                                                            \
      bool put = true;                                                         \
      while (tx_count && put) {                                                \
        put = HWREG(base + SSI_O_SR) & SSI_TX_NOT_FULL;                        \
        if (put) {                                                             \
          HWREG(base + SSI_O_DR) = *tx;                                        \
          tx += tx_inc;                                                        \
          tx_count--;                                                          \
        }                                                                      \
      }                                                                        \
      while (!(HWREG(base + SSI_O_SR) & SSI_RX_NOT_EMPTY)) {                   \
      }                                                                        \
      *rx = HWREG(base + SSI_O_DR);                                            \
      rx += rx_inc;                                                            \
    }                                                                          \
    while (HWREG(base + SSI_O_SR) & SSI_SR_BSY) {                              \
    }                                                                          \
  }

static uint16_t _scratch;

void spi_transfer_8(spi_t spi, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  ASSERT(spi < MAX_SPI);
  uint32_t base = _spi_lut[spi].base;
  uint32_t tx_inc = 1, rx_inc = 1;
  if (!tx) { tx = (uint8_t*)&_scratch; tx_inc = 0; }
  if (!rx) { rx = (uint8_t*)&_scratch; rx_inc = 0; }
  SPI_TRANSFER_LOOP(base, tx, tx_inc, rx, rx_inc, len);
}

void spi_transfer_16(spi_t spi, const uint16_t *tx, uint16_t *rx, uint32_t len)
{
  ASSERT(spi < MAX_SPI);
  uint32_t base = _spi_lut[spi].base;
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

//
// DMA methods
// 

#define BASIC_8 (UDMA_MODE_BASIC | UDMA_SIZE_8)
#define BASIC_16 (UDMA_MODE_BASIC | UDMA_SIZE_16)

static const uint32_t _dma_null_options[2] = {
  BASIC_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4,
  BASIC_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4,
};

static const uint32_t _dma_rx_options[2] = {
  BASIC_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_ARB_4,
  BASIC_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_ARB_4,
};

static const uint32_t _dma_tx_options[2] = {
  BASIC_8 | UDMA_SRC_INC_8 | UDMA_DST_INC_NONE | UDMA_ARB_4,
  BASIC_16 | UDMA_SRC_INC_16 | UDMA_DST_INC_NONE | UDMA_ARB_4,
};

#undef BASIC_8
#undef BASIC_16

static inline tDMAControlTable* dma_channel_entry(uint32_t channel)
{
  tDMAControlTable *dma_tbl = (tDMAControlTable *)HWREG(UDMA0_BASE + UDMA_O_CTRL);
  return &dma_tbl[channel];
}

static inline void enable_dma_channel(uint32_t channel_mask)
{  
  HWREG(UDMA0_BASE + UDMA_O_SETCHANNELEN) = channel_mask;
}

static inline void disable_dma_channel(uint32_t channel_mask)
{  
  HWREG(UDMA0_BASE + UDMA_O_CLEARCHANNELEN) = channel_mask;
}

static inline bool is_dma_done(uint32_t channel_mask)
{
  return HWREG(UDMA0_BASE + UDMA_O_REQDONE) & channel_mask;
}

static inline void clear_dma_done(uint32_t channel_mask)
{
  HWREG(UDMA0_BASE + UDMA_O_REQDONE) = channel_mask;
}

static inline void _start_rx_dma_xfer(uint32_t base, uint32_t dma_channel,
                                      void *buffer, uint32_t len,
                                      uint32_t data_size)
{
  uint32_t ctrl;

  tDMAControlTable *dma_entry = dma_channel_entry(dma_channel);
  dma_entry->pvSrcEndAddr = (void *)(base + SSI_O_DR);

  if (buffer) {
    ctrl = _dma_rx_options[data_size - 1];
    dma_entry->pvDstEndAddr = buffer + (len - 1) * data_size;
  } else {
    ctrl = _dma_null_options[data_size - 1];
    dma_entry->pvDstEndAddr = &_scratch;
  }

  ctrl |= (len - 1) << UDMA_XFER_SIZE_S;
  dma_entry->ui32Control = ctrl;
}

static inline void _start_tx_dma_xfer(uint32_t base, uint32_t dma_channel,
                                      const void *buffer, uint32_t len,
                                      uint32_t data_size)
{
  uint32_t ctrl;

  tDMAControlTable *dma_entry = dma_channel_entry(dma_channel);
  dma_entry->pvDstEndAddr = (void *)(base + SSI_O_DR);

  if (buffer) {
    ctrl = _dma_tx_options[data_size - 1];
    dma_entry->pvSrcEndAddr = (void*)buffer + (len - 1) * data_size;
  } else {
    ctrl = _dma_null_options[data_size - 1];
    dma_entry->pvSrcEndAddr = &_scratch;
  }

  ctrl |= (len - 1) << UDMA_XFER_SIZE_S;
  dma_entry->ui32Control = ctrl;
}

static void _spi_irq(spi_t spi)
{
  const spi_lut_t* lut = &_spi_lut[spi];
  uint32_t base = lut->base;

  uint32_t status = SSIIntStatus(base, true);
  SSIIntClear(base, status);

  if (is_dma_done(lut->tx_dma.mask)) {
    disable_dma_channel(lut->tx_dma.mask);
    SSIDMADisable(base, SSI_DMA_TX);
    clear_dma_done(lut->tx_dma.mask);
  }

  if (is_dma_done(lut->rx_dma.mask)) {
    disable_dma_channel(lut->rx_dma.mask);
    SSIDMADisable(base, SSI_DMA_RX);
    clear_dma_done(lut->rx_dma.mask);
    _spi_dma_active[spi] = false;
  }
}

// len in number of transfers
// data_size is 1 or 2
static void spi_transfer_dma(spi_t spi, const void *tx, void *rx, uint32_t len,
                             uint32_t data_size, bool blocking)
{
  ASSERT(uart < MAX_SPI);

  // wait until previous transfer is done
  while (_spi_dma_active[spi]) {}
 
  const spi_lut_t* lut = &_spi_lut[spi];
  uint32_t base = lut->base;
  uint32_t rx_channel = lut->rx_dma.channel;
  uint32_t tx_channel = lut->tx_dma.channel;

  _spi_dma_active[spi] = true;
  _start_rx_dma_xfer(base, rx_channel, rx, len, data_size);
  _start_tx_dma_xfer(base, tx_channel, tx, len, data_size);

  enable_dma_channel(lut->rx_dma.mask | lut->tx_dma.mask);
  SSIDMAEnable(base, SSI_DMA_TX | SSI_DMA_RX);

  if (blocking) {
    // wait until transfer is done
    while (_spi_dma_active[spi]) {}
  }
}

void spi_wait_dma_done(spi_t spi)
{
  ASSERT(uart < MAX_SPI);
  while (_spi_dma_active[spi]) {}
}

void spi_read_dma_8(spi_t spi, uint8_t* data, uint32_t len, bool blocking)
{
  spi_transfer_dma(spi, 0, data, len, sizeof(uint8_t), blocking);
}

void spi_read_dma_16(spi_t spi, uint16_t* data, uint32_t len, bool blocking)
{
  spi_transfer_dma(spi, 0, data, len, sizeof(uint16_t), blocking);
}

void spi_write_dma_8(spi_t spi, const uint8_t* data, uint32_t len, bool blocking)
{
  spi_transfer_dma(spi, data, 0, len, sizeof(uint8_t), blocking);
}

void spi_write_dma_16(spi_t spi, const uint16_t* data, uint32_t len, bool blocking)
{
  spi_transfer_dma(spi, data, 0, len, sizeof(uint16_t), blocking);
}

