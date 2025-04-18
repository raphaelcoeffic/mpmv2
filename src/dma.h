#pragma once

#include <driverlib/udma.h>

// offset between primrary and alternate entry
#define DMA_ALT_OFFSET 128

void dma_init();

static inline tDMAControlTable* dma_channel_entry(uint32_t channel)
{
  tDMAControlTable *dma_tbl = (tDMAControlTable *)HWREG(UDMA0_BASE + UDMA_O_CTRL);
  return &dma_tbl[channel];
}

static inline tDMAControlTable* dma_channel_alt_entry(uint32_t channel)
{
  return dma_channel_entry(channel + DMA_ALT_OFFSET);
}
