#include <driverlib/prcm.h>
#include <driverlib/udma.h>

static uint8_t _dma_ctrl_tbl[1024] __attribute__((aligned(1024)));

void dma_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UDMA);
  PRCMLoadSet();

  uDMAEnable(UDMA0_BASE);
  uDMAControlBaseSet(UDMA0_BASE, _dma_ctrl_tbl);
}
