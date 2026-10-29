#include "rf.h"

#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_ckmd.h)
#include DeviceFamily_constructPath(inc/hw_lrfddbell.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm.h)
#include DeviceFamily_constructPath(inc/hw_systim.h)
#include DeviceFamily_constructPath(inc/pbe_generic_ram_regs.h)

// PBE events (LRFDDBELL RIS0)
#define EV_OPDONE (1U << 0)
#define EV_RXNOK (1U << 4)
#define EV_RXOK (1U << 8)
#define EV_OPERROR (1U << 15)
#define EV_SYSTIM0 LRFDDBELL_RIS0_SYSTIM0_M

// SYSTIM: 0.25 us ticks
#define US(x) ((x) * 4U)
// Start of an operation: some margin for the PBE to be ready (TI: 50 us)
#define START_DELAY US(100)

#define BUFRAM(reg) (LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_##reg)

static LRF_RadioState _state = RadioState_Down;
static const LRF_Config *_config;
static LRF_TxPowerTable_Index _power = {.rawValue = 0};
static uint16_t _endcause;
static uint32_t _events;

void (*rf_debug)(int step, uint32_t events, int8_t rssi, uint16_t endcause);

static uint32_t _now(void) { return HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N); }

void rf_init(void)
{
  LRF_rclEnableRadioClocks();
  // The HF clock buffer feeding the synthesizer
  HWREG(CKMD_BASE + CKMD_O_HFXTCTL) |= CKMD_HFXTCTL_HPBUFEN;
  _state = RadioState_Down;
}

// RX: the PBE only ends a packet on a CRC verdict (with no CRC bits it
// never does: the entry stays uncommitted, the operation hangs). 8 CRC bits
// then, the PHY's polynomial: the packet's last byte, appended to the FIFO
// entry (APPENDCRC), the entry kept whatever the verdict. The packet's RSSI
// follows (APPENDRSSI, 2 bytes: LASTRSSI is only for packets with a good
// CRC). EXTRABYTES: their room, in the entry's length
#define RX_CRC_BITS 8
#define RX_EXTRA_BYTES (RX_CRC_BITS / 8 + 2)

// Raw framing over the PHY's settings: no header, length field, CRC (see
// RX_CRC_BITS), whitening, NESB; payloads of a fixed length (MAXLEN); bit
// orders as the PHY (NESB: MSB first)
static void _raw_framing(void)
{
  uint16_t pktcfg = HWREGH_READ_LRF(BUFRAM(PKTCFG));
  pktcfg &= ~(PBE_GENERIC_RAM_PKTCFG_NUMHDRBITS_M | PBE_GENERIC_RAM_PKTCFG_CRCINCSW_M |
              PBE_GENERIC_RAM_PKTCFG_CRCINCHDR_M);
  HWREGH_WRITE_LRF(BUFRAM(PKTCFG)) = pktcfg;
  HWREGH_WRITE_LRF(BUFRAM(NUMCRCBITS)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(WHITEINIT)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(LENCFG)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(LENOFFSET)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(ADDRCFG)) = 0;
  uint16_t fifocfg = HWREGH_READ_LRF(BUFRAM(FIFOCFG));
  fifocfg &= ~(0xFC00U | PBE_GENERIC_RAM_FIFOCFG_AUTOFLUSHCRC_M | PBE_GENERIC_RAM_FIFOCFG_AUTOFLUSHEMPTY_M);
  HWREGH_WRITE_LRF(BUFRAM(FIFOCFG)) =
      fifocfg | PBE_GENERIC_RAM_FIFOCFG_APPENDCRC | PBE_GENERIC_RAM_FIFOCFG_APPENDRSSI;
  HWREGH_WRITE_LRF(BUFRAM(EXTRABYTES)) = RX_EXTRA_BYTES;
  // Not NESB (the NESB PHY turns it on): in RX the PBE would take a PCF and
  // answer with an ACK
  HWREGH_WRITE_LRF(BUFRAM(NESB)) = HWREGH_READ_LRF(BUFRAM(NESB)) & ~PBE_GENERIC_RAM_NESB_NESBMODE_M;
}

int rf_setup(const LRF_Config *config)
{
  // Another PHY: its images and every register again
  LRF_RadioState state = _state;
  if (config != _config && state > RadioState_ImagesLoaded) state = RadioState_ImagesLoaded;
  LRF_SetupResult res = LRF_setupRadio(config, LRF_PhyFeatures_Default, state);
  if (res != SetupResult_Ok) {
    _state = RadioState_Down;
    _config = NULL;
    return res;
  }
  _state = RadioState_Configured;
  _config = config;
  _raw_framing();
  return 0;
}

void rf_set_sync(uint32_t sync, uint8_t bits)
{
  // REFLEN: sync length - 1; LRF right-aligns the word to it (and reverses
  // it for HDRORDER)
  uint32_t demswqu0 = HWREG_READ_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMSWQU0);
  demswqu0 = (demswqu0 & ~LRFDMDM_DEMSWQU0_REFLEN_M) |
             (((uint32_t)(bits - 1) << LRFDMDM_DEMSWQU0_REFLEN_S) & LRFDMDM_DEMSWQU0_REFLEN_M);
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMSWQU0) = demswqu0;
  // B as A, as TI's NESB PTX: NESB searches both (PRX: two pipes); with B
  // not programmed the MCE never starts its demodulator (MDM ENABLE stays
  // TOPSM|FIFO, the RFE never acks the RX start)
  LRF_Interface_Generic_programSyncWordA(sync);
  LRF_Interface_Generic_programSyncWordB(sync);
}

int rf_set_power(int8_t dbm)
{
  _power.rawValue = dbm * 2;
  return 0;
}

static void _clear_events(uint32_t mask)
{
  HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_ICLR0) = mask;
}

static uint32_t _events_now(void) { return HWREG_READ_LRF(LRFDDBELL_BASE + LRFDDBELL_O_RIS0); }

// The PBE starts the operation at LRFD event SYSTIM0: SYSTIM channel 2
static void _start_at(uint32_t time)
{
  _clear_events(EV_SYSTIM0);
  HWREG(SYSTIM_BASE + SYSTIM_O_CH2CC) = time;
}

static void _cancel_systim0(void)
{
  HWREG(SYSTIM_BASE + SYSTIM_O_CH2CC) = 0;
  HWREG(SYSTIM_BASE + SYSTIM_O_CH2CFG) = SYSTIM_CH2CFG_MODE_CAPT;
  HWREG(SYSTIM_BASE + SYSTIM_O_CH2CFG) = SYSTIM_CH2CFG_MODE_DIS;
  _clear_events(EV_SYSTIM0);
}

// Waits for OPDONE / OPERROR until the deadline (SYSTIM)
static int _wait_done(uint32_t deadline)
{
  for (;;) {
    _events = _events_now();
    if (_events & (EV_OPDONE | EV_OPERROR)) break;
    if ((int32_t)(_now() - deadline) > 0) {
      LRF_sendHardStop();
      uint32_t t = _now();
      while (!(_events_now() & (EV_OPDONE | EV_OPERROR)) && _now() - t < US(1000)) {
      }
      _events = _events_now();
      _endcause = LRF_Interface_getCmdEndCause();
      return RF_ERR_TIMEOUT;
    }
  }
  _endcause = LRF_Interface_getCmdEndCause();
  return _endcause;
}

static void _end(void)
{
  _cancel_systim0();
  _clear_events(0xFFFFFFFFU);
  LRF_disable();
  LRF_disableSynthRefsys();
}

int rf_tx(uint32_t freq_hz, const uint8_t *data, uint8_t len)
{
  if (_state != RadioState_Configured) return RF_ERR_SETUP;
  if (len > RF_MAX_PAYLOAD) len = RF_MAX_PAYLOAD;

  uint32_t earliest = LRF_enableSynthRefsys();
  HWREGH_WRITE_LRF(BUFRAM(NUMCRCBITS)) = 0;
  LRF_Interface_Generic_configOpTx(1, freq_hz);
  LRF_programFrequency(freq_hz, true);
  LRF_programTxPower(_power, freq_hz);
  LRF_enable();

  // FIFO entry: length (after the field), one pad byte, the packet
  uint32_t entry[(RF_MAX_PAYLOAD + 4 + 3) / 4];
  uint8_t *b = (uint8_t *)entry;
  uint16_t length = 2 + len;
  b[0] = length & 0xFF;
  b[1] = length >> 8;
  b[2] = 1;  // numPad
  b[3] = 0;  // pad
  memcpy(b + 4, data, len);
  LRF_prepareTxFifo();
  LRF_writeTxFifoWords(entry, (len + 4 + 3) / 4);

  _clear_events(0xFFFFFFFFU);
  uint32_t start = _now() + START_DELAY;
  if ((int32_t)(earliest - start) > 0) start = earliest;
  _start_at(start);
  LRF_waitForTopsmReady();
  LRF_Interface_Generic_sendOpTx();

  int res = _wait_done(start + US(10000));
  _end();
  return res;
}

int rf_rx(uint32_t freq_hz, uint8_t len, uint32_t timeout_us, rf_packet_t *pkt)
{
  if (_state != RadioState_Configured) return RF_ERR_SETUP;
  if (len > RF_MAX_PAYLOAD) len = RF_MAX_PAYLOAD;
  if (len < 2) len = 2;

  uint32_t earliest = LRF_enableSynthRefsys();
  // One packet, then OPDONE: len - 1 bytes and the CRC byte
  HWREGH_WRITE_LRF(BUFRAM(NUMCRCBITS)) = RX_CRC_BITS;
  LRF_Interface_Generic_configOpRx(1, freq_hz, 0, len - RX_CRC_BITS / 8);
  LRF_programFrequency(freq_hz, false);
  LRF_enable();
  LRF_prepareRxFifo();
  LRF_setRxFifoEffSz(256);
  HWREGH_WRITE_LRF(BUFRAM(NRXOK)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(NRXNOK)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(NRXIGNORED)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(NRXEMPTY)) = 0;
  HWREGH_WRITE_LRF(BUFRAM(NRXFIFOFULL)) = 0;

  _clear_events(0xFFFFFFFFU);
  uint32_t start = _now() + START_DELAY;
  if ((int32_t)(earliest - start) > 0) start = earliest;
  _start_at(start);
  LRF_waitForTopsmReady();
  LRF_Interface_Generic_sendOpRx();

  // The PBE takes SYSTIM0 as the start; the stop is ours (hard stop)
  if (rf_debug) {
    for (int i = 0; i < 8; i++) {
      uint32_t t = _now();
      while (_now() - t < US(timeout_us / 8)) {
      }
      rf_debug(i, _events_now(), LRF_readRssi(), HWREGH_READ_LRF(LRFD_BUFRAM_BASE + 6));
    }
  }
  int res = _wait_done(start + US(timeout_us));
  pkt->len = 0;
  if (_events & (EV_RXOK | EV_RXNOK)) {
    if (LRF_hasRxWordToRead()) {
      // Entry: length (from numPad on), numPad, the pad bytes, the packet,
      // EXTRABYTES (the CRC byte, the RSSI)
      uint32_t w = LRF_peekRxFifo(0);
      uint32_t entry_len = w & 0xFFFF;
      uint32_t words = (2 + entry_len + 3) / 4;
      uint32_t buf[(RF_MAX_PAYLOAD + 16) / 4];
      if (words <= sizeof(buf) / 4) {
        LRF_readRxFifoWords(buf, words);
        uint8_t *b = (uint8_t *)buf;
        uint8_t num_pad = b[2];
        int n = (int)entry_len - 1 - num_pad - RX_EXTRA_BYTES + RX_CRC_BITS / 8;
        if (n > 0 && n <= RF_MAX_PAYLOAD) {
          memcpy(pkt->data, b + 3 + num_pad, n);
          pkt->len = n;
          pkt->rssi = (int8_t)b[3 + num_pad + n];
        }
      } else {
        LRF_discardRxFifoWords(words);
      }
    }
    pkt->time = LRF_Interface_Generic_getLastPacketTimestamp();
    res = RF_OK;
  } else if (res == RF_ERR_TIMEOUT) {
    res = RF_ERR_NO_PACKET;
  }
  _end();
  return res;
}

void rf_stats(rf_stats_t *stats)
{
  stats->rx_ok = HWREGH_READ_LRF(BUFRAM(NRXOK));
  stats->rx_nok = HWREGH_READ_LRF(BUFRAM(NRXNOK));
  stats->rx_ignored = HWREGH_READ_LRF(BUFRAM(NRXIGNORED));
  stats->rx_empty = HWREGH_READ_LRF(BUFRAM(NRXEMPTY));
  stats->rx_fifo_full = HWREGH_READ_LRF(BUFRAM(NRXFIFOFULL));
  stats->rx_timeout = HWREGH_READ_LRF(BUFRAM(NRXTIMEOUT));
  stats->last_rssi = (int8_t)HWREGH_READ_LRF(BUFRAM(LASTRSSI));
}

uint16_t rf_last_endcause(void) { return _endcause; }
uint32_t rf_last_events(void) { return _events; }
