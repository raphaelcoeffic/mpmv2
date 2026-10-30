#include "phy_fsk.h"

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm.h)
#include <ti/drivers/rcl/LRF.h>

#include "phy_fsk250.h"
#include "phy_nesb.h"
#include "rf.h"

// Rate = 48 MHz x rate word (21 bits) / (2^21 x prescaler)
#define BAUD_CLOCK 48000000U
// Prescaler clock per symbol, about TI's (250 kbps: 15, 12.8 clocks), the
// prescaler 12 at least (TI's from 500 kbps; with 2 at 2 Mbps nothing
// received)
#define BAUD_CLOCKS_PER_SYMBOL 12U
#define PRESCALER_MIN 12U
// Above: TI's NESB 2 Mbps PHY (ble5 images, as TI's CC23xx 2 Mbps GFSK);
// the genfsk images don't transmit at 2 Mbps
#define GENFSK_MAX_RATE 1200000U

// Samples' rate after CMIX (f_synth / 288) at the band's lowest synthesizer
// frequency, a margin taken: the resampler's output (4 x rate x PDIF) must
// stay below the decimators' (LRF_programPQ). TI's PHYs: 2 Mbps BDE2 1,
// 1 Mbps 2, 500 kbps 4, 250 kbps 8
#define FS_CMIX (2350000000U / 288U)
#define SAMPLES_PER_SYMBOL 4U
// Nothing received beyond (whatever the digital gains): BDE2 above 8, PDIF
// decimation above 2, BDE1 decimating. The resampler takes the rest (P/Q
// 0.48 at 250 kbps BDE2 8 is fine)
#define MAX_BDE2 8U
#define MAX_LOG2_PDIF 1

// TX pulse for 2-FSK. LRF's TX filter: a symmetric FIR of 48 taps at 16
// per symbol, the base PHYs' tables its last 24 (TI's Gaussian BT 0.5:
// rectangle filtered, 20 taps). The scale: 2^28 / sum of the 48 taps, the
// deviation then the long run's
#define SHAPE_SAMPLES_PER_SYMBOL 16
static const LRF_TxShape _shape_rect = {
    .scale = (1U << 28) / (SHAPE_SAMPLES_PER_SYMBOL * 255),
    .numCoeff = SHAPE_SAMPLES_PER_SYMBOL / 2,
    .coeff = {255, 255, 255, 255, 255, 255, 255, 255},
};

static LRF_SwConfig _sw;

static uint32_t _div_round(uint64_t a, uint32_t b) { return (uint32_t)((a + b / 2) / b); }

int phy_fsk_setup(const phy_fsk_t *phy, phy_fsk_info_t *info)
{
  int res = rf_setup(phy->rate > GENFSK_MAX_RATE ? &phy_nesb_2_mbps : &phy_fsk250_common);
  if (res) return res;

  // Deviation: LRF programs the TX shape from it at each frequency change
  LRF_SwParam *sw = (LRF_SwParam *)swParamList;
  _sw = *sw->swConfig;
  _sw.modFrequencyDeviation = phy->deviation;
  if (phy->fsk) _sw.txShape = &_shape_rect;
  sw->swConfig = &_sw;

  uint32_t pre = _div_round(BAUD_CLOCK, phy->rate * BAUD_CLOCKS_PER_SYMBOL);
  if (pre < PRESCALER_MIN) pre = PRESCALER_MIN;
  if (pre > 255) pre = 255;
  uint32_t rw = _div_round((uint64_t)phy->rate * pre << 21, BAUD_CLOCK);
  if (rw >= 1U << 21) rw = (1U << 21) - 1;
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_BAUD) = rw >> 5;
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_BAUDPRE) =
      (HWREG_READ_LRF(LRFDMDM_BASE + LRFDMDM_O_BAUDPRE) & LRFDMDM_BAUDPRE_ALIGNVALUE_M) |
      (rw & 0x1F) << LRFDMDM_BAUDPRE_EXTRATEWORD_S | pre << LRFDMDM_BAUDPRE_PRESCALER_S;

  // PDIF decimation: the samples' rate before the resampler at least twice
  // the signal's Carson bandwidth (2 x deviation + rate). TI's 250 kbps
  // (62.5 kHz) needs none; with 160 kHz (NRF24 at 250 kbps) it lost packets
  int log2_pdif = phy->log2_pdif;
  if (log2_pdif < 0) {
    uint32_t carson = 2 * phy->deviation + phy->rate;
    for (log2_pdif = 0; log2_pdif < MAX_LOG2_PDIF; log2_pdif++)
      if ((SAMPLES_PER_SYMBOL * phy->rate << log2_pdif) >= 2 * carson) break;
  }
  if (log2_pdif > MAX_LOG2_PDIF) log2_pdif = MAX_LOG2_PDIF;

  // Decimators: as much as the resampler allows, up to BDE2 8
  uint32_t fs_res = SAMPLES_PER_SYMBOL * phy->rate << log2_pdif;
  uint32_t bde1 = 1;
  uint32_t bde2 = FS_CMIX / fs_res;
  if (bde2 > MAX_BDE2) bde2 = MAX_BDE2;
  if (bde2 < 1) bde2 = 1;

  uint32_t demmisc3 = HWREG_READ_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMMISC3);
  demmisc3 &= ~(LRFDMDM_DEMMISC3_BDE1FILTMODE_M | LRFDMDM_DEMMISC3_BDE2DVGA_M |
                LRFDMDM_DEMMISC3_BDE1DVGA_M | LRFDMDM_DEMMISC3_PDIFDECIM_M |
                LRFDMDM_DEMMISC3_BDE2DECRATIO_M);
  // BDE1: TI's PHYs all filter only (no decimation)
  if (bde1 == 1) demmisc3 |= LRFDMDM_DEMMISC3_BDE1FILTMODE_M;
  // Digital gains: as TI's, BDE1 1 when decimating at all, BDE2 2 from 4
  if (bde1 * bde2 > 1) demmisc3 |= 1U << LRFDMDM_DEMMISC3_BDE1DVGA_S;
  if (bde2 >= 4) demmisc3 |= 2U << LRFDMDM_DEMMISC3_BDE2DVGA_S;
  demmisc3 |= (uint32_t)log2_pdif << LRFDMDM_DEMMISC3_PDIFDECIM_S | bde2 << LRFDMDM_DEMMISC3_BDE2DECRATIO_S;
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMMISC3) = demmisc3;
  // TX: the PDIF decimation slows the bits by as much
  rf_set_demmisc3(demmisc3 & ~LRFDMDM_DEMMISC3_PDIFDECIM_M, demmisc3);

  if (info) {
    info->rate = (uint32_t)(((uint64_t)BAUD_CLOCK * rw >> 21) / pre);
    info->prescaler = pre;
    info->rate_word = rw;
    info->bde1 = bde1;
    info->bde2 = bde2;
    info->pdif = 1 << log2_pdif;
    info->demmisc3 = demmisc3;
  }
  return 0;
}
