#include "rf.h"

#include <string.h>

#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/rcl/lrf/LRFLPF3.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_ckmd.h)
#include DeviceFamily_constructPath(inc/hw_lrfddbell.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm.h)
#include DeviceFamily_constructPath(inc/hw_lrfdpbe.h)
#include DeviceFamily_constructPath(inc/hw_systim.h)
#include DeviceFamily_constructPath(inc/pbe_common_ram_regs.h)
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
// The same with the radio on and tuned (rf_fs_start()): the PBE takes the
// op at once
#define START_DELAY_TUNED US(30)
// Hard stop: the PBE ends the operation within (measured: some us)
#define STOP_TIMEOUT US(1000)

#define BUFRAM(reg) (LRFD_BUFRAM_BASE + PBE_GENERIC_RAM_O_##reg)

static LRF_RadioState _state = RadioState_Down;
static const LRF_Config *_config;
static LRF_TxPowerTable_Index _power = {.rawValue = 0};
static uint16_t _endcause;
static rf_modem_t _modem_tx, _modem_rx;
static uint32_t _events;
static bool _running;
static uint32_t _rx_fifo_size;
// OP_FS: the synthesizer tuned ahead for a TX at _fs_freq (rf_fs_start()),
// kept on once locked (OPDONE)
static bool _fs_on;
static uint32_t _fs_freq;

// TX: the packet's bytes not in the FIFO yet (long packets)
static const uint8_t *_tx_data;
static uint16_t _tx_left;

// Long RX (more than RF_FIFO_PAYLOAD): one packet per operation, copied out
// of the FIFO as the PBE writes it (_rx_copy()), RX started again after each
// (repeat)
static bool _rx_long, _rx_active, _rx_repeat;
static uint32_t _rx_freq;
static uint16_t _rx_len;
static uint16_t _rx_pos;  // FIFO index of the next byte to copy
static uint16_t _rx_got;  // bytes copied
static uint32_t _rx_buf[(RF_MAX_PAYLOAD + 16 + 3) / 4];
static rf_packet_t _rx_pkt;  // the last one, until rf_rx_read()
static bool _rx_ready;

void (*rf_debug)(int step, uint32_t events, int8_t rssi, uint16_t endcause);

static uint32_t _now(void) { return HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N); }

uint32_t rf_time(void) { return _now(); }

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
// entry (APPENDCRC), the entry kept whatever the verdict. Then the
// frequency estimate, the RSSI (a byte each; LASTRSSI and LASTFREQOFF are
// only for packets with a good CRC) and the SYSTIM time of the sync word (4
// bytes, from the capture on SYSTIM channel 4). EXTRABYTES: their room, all
// of it in the entry's length
#define RX_CRC_BITS 8
#define RX_EXTRA_BYTES (RX_CRC_BITS / 8 + 1 + 1 + 4)

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
      fifocfg | PBE_GENERIC_RAM_FIFOCFG_APPENDCRC | PBE_GENERIC_RAM_FIFOCFG_APPENDFREQEST |
      PBE_GENERIC_RAM_FIFOCFG_APPENDRSSI | PBE_GENERIC_RAM_FIFOCFG_APPENDTIMESTAMP;
  HWREGH_WRITE_LRF(BUFRAM(EXTRABYTES)) = RX_EXTRA_BYTES;
  // Not NESB (the NESB PHY turns it on): in RX the PBE would take a PCF and
  // answer with an ACK
  HWREGH_WRITE_LRF(BUFRAM(NESB)) = HWREGH_READ_LRF(BUFRAM(NESB)) & ~PBE_GENERIC_RAM_NESB_NESBMODE_M;
}

// RX FIFO entry: length (from numPad on), numPad, the pad bytes, the packet,
// EXTRABYTES (the CRC byte, frequency estimate, RSSI, timestamp). False if
// it doesn't fit in n bytes or pkt
static bool _parse_entry(const uint8_t *b, uint32_t n, rf_packet_t *pkt)
{
  uint32_t entry_len = b[0] | b[1] << 8;
  uint8_t num_pad = b[2];
  int len = (int)entry_len - 1 - num_pad - RX_EXTRA_BYTES + RX_CRC_BITS / 8;
  if (len <= 0 || len > RF_MAX_PAYLOAD || 2 + entry_len > n) return false;
  const uint8_t *p = b + 3 + num_pad;
  memcpy(pkt->data, p, len);
  pkt->len = len;
  pkt->freq_est = (int8_t)p[len];
  pkt->rssi = (int8_t)p[len + 1];
  pkt->time = p[len + 2] | p[len + 3] << 8 | p[len + 4] << 16 | (uint32_t)p[len + 5] << 24;
  return true;
}

// A FIFO pointer write, as TI's LRF_writeFifoPtr() (RCL-367: the PBE's FIFO
// commands ignored meanwhile). It moves the pointer forward (to the same
// value: a full round)
static void _write_fifo_ptr(uint32_t reg, uint32_t value)
{
  uintptr_t key = HwiP_disable();
  HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_COMMON_RAM_O_FIFOCMDADD) =
      ((LRFDPBE_BASE + LRFDPBE_O_FSTAT) & 0x0FFFU) >> 2U;
  (void)HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_COMMON_RAM_O_FIFOCMDADD);
  (void)HWREGH_READ_LRF(LRFD_BUFRAM_BASE + PBE_COMMON_RAM_O_FIFOCMDADD);
  HWREG_WRITE_LRF(reg) = value;
  HWREGH_WRITE_LRF(LRFD_BUFRAM_BASE + PBE_COMMON_RAM_O_FIFOCMDADD) =
      ((LRFDPBE_BASE + LRFDPBE_O_FCMD) & 0x0FFFU) >> 2U;
  HwiP_restore(key);
}

// TX: what fits of the packet's rest (whole words, the last one padded).
// The PBE frees what it has sent (TXADEAL)
static void _tx_fill(void)
{
  while (_tx_left) {
    uint32_t words = LRF_getTxFifoWritable() / 4;
    if (!words) return;
    uint32_t buf[16];
    if (words > 16) words = 16;
    uint32_t n = _tx_left < words * 4 ? _tx_left : words * 4;
    memcpy(buf, _tx_data, n);
    LRF_writeTxFifoWords(buf, (n + 3) / 4);
    _tx_data += n;
    _tx_left -= n;
  }
}

// Long RX: copies what the PBE wrote since (whole words, all of it at the
// end) and lets it write up to a FIFO size after that, less a word (the RX
// FIFO's write limit, SRP: TI's LRF_setRxFifoEffSz() moves it the same way).
// The PBE's write pointer runs ahead of the commit (end of the packet)
static void _rx_copy(bool end)
{
  uint32_t size = _rx_fifo_size;
  uint32_t wp = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_RXFWP);
  uint32_t n = (wp + size - _rx_pos) % size;
  if (!end) n &= ~3U;
  if (n > sizeof(_rx_buf) - _rx_got) n = sizeof(_rx_buf) - _rx_got;
  if (!n) return;
  uint32_t start = (HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG3) & LRFDPBE_FCFG3_RXSTRT_M) << 2;
  // The FIFO's unwrapped view: reads go on past its end from its start
  volatile const uint32_t *src =
      (volatile const uint32_t *)(RXF_UNWRAPPED_BASE_ADDR + start + _rx_pos);
  uint32_t *dst = _rx_buf + _rx_got / 4;
  ASM_4_NOPS();  // RCL-515, as TI's FIFO reads
  for (uint32_t i = 0; i < (n + 3) / 4; i++) dst[i] = src[i];
  _rx_got += n;
  _rx_pos = (_rx_pos + n) % size;
  if (!end) _write_fifo_ptr(LRFDPBE_BASE + LRFDPBE_O_RXFSRP, (_rx_pos + size - 4) % size);
}

int rf_configure(const LRF_Config *config)
{
  rf_stop();
  // Every register again (the same PHY too: phy_fsk changes some after)
  LRF_RadioState state = _state;
  if (state > RadioState_ImagesLoaded) state = RadioState_ImagesLoaded;
  LRF_SetupResult res = LRF_setupRadio(config, LRF_PhyFeatures_Default, state);
  if (res != SetupResult_Ok) {
    _state = RadioState_Down;
    _config = NULL;
    return res;
  }
  _state = RadioState_Configured;
  _config = config;
  _modem_tx = _modem_rx = (rf_modem_t){0};
  _raw_framing();
  return 0;
}

void rf_set_modem(const rf_modem_t *tx, const rf_modem_t *rx)
{
  _modem_tx = *tx;
  _modem_rx = *rx;
}

// Before LRF_programFrequency(), which takes the decimation and rate from
// them
static void _modem(const rf_modem_t *m)
{
  if (m->demmisc3) HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMMISC3) = m->demmisc3;
  if (m->baud) HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_BAUD) = m->baud;
  if (m->baudpre) HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_BAUDPRE) = m->baudpre;
}

void rf_set_syncs(uint32_t sync_a, uint32_t sync_b, uint8_t bits)
{
  // REFLEN: sync length - 1; LRF right-aligns the word to it (and reverses
  // it for HDRORDER)
  uint32_t demswqu0 = HWREG_READ_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMSWQU0);
  demswqu0 = (demswqu0 & ~LRFDMDM_DEMSWQU0_REFLEN_M) |
             (((uint32_t)(bits - 1) << LRFDMDM_DEMSWQU0_REFLEN_S) & LRFDMDM_DEMSWQU0_REFLEN_M);
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_DEMSWQU0) = demswqu0;
  // TX sends A. B: with B not programmed the MCE never starts its
  // demodulator (MDM ENABLE stays TOPSM|FIFO, the RFE never acks the RX
  // start); NESB searches both (PRX: two pipes)
  LRF_Interface_Generic_programSyncWordA(sync_a);
  LRF_Interface_Generic_programSyncWordB(sync_b);
}

void rf_set_sync(uint32_t sync, uint8_t bits) { rf_set_syncs(sync, sync, bits); }

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

// The start time: at (0: as soon as possible), not before the synthesizer's
// reference is ready (earliest) nor the PBE (delay: START_DELAY*)
static uint32_t _start_time(uint32_t at, uint32_t earliest, uint32_t delay)
{
  uint32_t start = _now() + delay;
  if (at && (int32_t)(at - start) > 0) start = at;
  if ((int32_t)(earliest - start) > 0) start = earliest;
  return start;
}

static void _end(void)
{
  _cancel_systim0();
  _clear_events(0xFFFFFFFFU);
  LRF_disable();
  LRF_disableSynthRefsys();
  _running = false;
}

static bool _op_busy(void)
{
  if (!_running) return false;
  _events = _events_now();
  return !(_events & (EV_OPDONE | EV_OPERROR));
}

static int _rx_start(uint32_t freq_hz, uint16_t len, bool repeat, uint32_t at, bool clear_stats);

// Long RX: the packet so far; at the end the entry parsed, RX started
// again (repeat)
static void _rx_pump(void)
{
  if (!_rx_active) return;
  uint32_t events = _events_now();
  bool end = events & (EV_OPDONE | EV_OPERROR);
  _rx_copy(end);
  if (!end) return;
  _rx_active = false;
  if (events & (EV_RXOK | EV_RXNOK))
    _rx_ready = _parse_entry((const uint8_t *)_rx_buf, _rx_got, &_rx_pkt);
  if (_rx_repeat) _rx_start(_rx_freq, _rx_len, true, 0, false);
}

bool rf_busy(void)
{
  if (!_running) return false;
  if (_tx_left) _tx_fill();
  _rx_pump();
  // A long packet that ended after the pump looked: busy until it's in
  if (_rx_active) return true;
  return _op_busy();
}

int rf_stop(void)
{
  _tx_left = 0;
  _rx_active = false;
  _fs_on = false;
  if (!_running) return RF_OK;
  int res = RF_OK;
  if (_op_busy()) {
    LRF_sendHardStop();
    uint32_t t = _now();
    while (!(_events_now() & (EV_OPDONE | EV_OPERROR)) && _now() - t < STOP_TIMEOUT) {
    }
    _events = _events_now();
    res = RF_ERR_TIMEOUT;
  }
  _endcause = LRF_Interface_getCmdEndCause();
  if (res == RF_OK) res = _endcause;
  _end();
  return res;
}

// Waits for OPDONE / OPERROR until the deadline (SYSTIM), stops the
// operation then
static int _wait_done(uint32_t deadline)
{
  while (rf_busy()) {
    if ((int32_t)(_now() - deadline) > 0) return rf_stop();
  }
  return rf_stop();
}

int rf_fs_start(uint32_t freq_hz)
{
  if (_state != RadioState_Configured) return RF_ERR_SETUP;
  rf_stop();
  _rx_long = _rx_ready = false;

  uint32_t earliest = LRF_enableSynthRefsys();
  LRF_Interface_Generic_configOpFs();
  _modem(&_modem_tx);
  LRF_programFrequency(freq_hz, true);
  LRF_enable();

  _clear_events(0xFFFFFFFFU);
  _start_at(_start_time(0, earliest, START_DELAY));
  LRF_waitForTopsmReady();
  LRF_Interface_Generic_sendOpFs();
  _running = true;
  _fs_on = true;
  _fs_freq = freq_hz;
  return RF_OK;
}

// The synthesizer of rf_fs_start() locked at freq_hz: its op done (waited
// for), the radio left on. Else false, the radio off
static bool _fs_ready(uint32_t freq_hz)
{
  bool on = _fs_on && _running && _fs_freq == freq_hz;
  _fs_on = false;
  if (on) {
    uint32_t t = _now();
    while (!(_events_now() & (EV_OPDONE | EV_OPERROR)) && _now() - t < STOP_TIMEOUT) {
    }
    _events = _events_now();
    on = (_events & (EV_OPDONE | EV_OPERROR)) == EV_OPDONE;
  }
  if (!on) {
    rf_stop();
    return false;
  }
  _endcause = LRF_Interface_getCmdEndCause();
  _cancel_systim0();
  _running = false;
  return true;
}

int rf_tx_start(uint32_t freq_hz, const uint8_t *data, uint16_t len, uint32_t at)
{
  if (_state != RadioState_Configured) return RF_ERR_SETUP;
  if (len > RF_MAX_PAYLOAD) return RF_ERR_PARAM;
  _tx_left = 0;
  _rx_active = false;
  // Tuned ahead: no calibration (frequency 0), the synthesizer off after
  bool fs = _fs_ready(freq_hz);
  _rx_long = _rx_ready = false;

  uint32_t earliest = fs ? _now() : LRF_enableSynthRefsys();
  HWREGH_WRITE_LRF(BUFRAM(NUMCRCBITS)) = 0;
  LRF_Interface_Generic_configOpTx(1, fs ? 0 : freq_hz);
  if (!fs) {
    _modem(&_modem_tx);
    LRF_programFrequency(freq_hz, true);
  }
  LRF_programTxPower(_power, freq_hz);
  if (!fs) LRF_enable();

  // FIFO entry: length (after the field), numPad 1, one pad byte, the
  // packet: what fits, the PBE freeing what it sends (rf_busy() refills)
  uint32_t hdr = (2U + len) | 1U << 16;
  LRF_prepareTxFifo();
  uint32_t fcfg0 = HWREG_READ_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0);
  HWREG_WRITE_LRF(LRFDPBE_BASE + LRFDPBE_O_FCFG0) = fcfg0 | LRFDPBE_FCFG0_TXADEAL;
  LRF_writeTxFifoWords(&hdr, 1);
  _tx_data = data;
  _tx_left = len;
  _tx_fill();

  _clear_events(0xFFFFFFFFU);
  _start_at(_start_time(at, earliest, fs ? START_DELAY_TUNED : START_DELAY));
  LRF_waitForTopsmReady();
  LRF_Interface_Generic_sendOpTx();
  _running = true;
  return RF_OK;
}

int rf_tx(uint32_t freq_hz, const uint8_t *data, uint16_t len)
{
  int res = rf_tx_start(freq_hz, data, len, 0);
  if (res) return res;
  return _wait_done(_now() + START_DELAY + US(10000) + US((uint32_t)len * 8 * 1000));
}

static int _rx_start(uint32_t freq_hz, uint16_t len, bool repeat, uint32_t at, bool clear_stats)
{
  if (_state != RadioState_Configured) return RF_ERR_SETUP;
  if (len > RF_MAX_PAYLOAD || len < 2) return RF_ERR_PARAM;
  rf_stop();
  _rx_long = len > RF_FIFO_PAYLOAD;
  _rx_repeat = repeat;
  _rx_freq = freq_hz;
  _rx_len = len;

  uint32_t earliest = LRF_enableSynthRefsys();
  // len - 1 bytes and the CRC byte. Long: one packet per operation
  HWREGH_WRITE_LRF(BUFRAM(NUMCRCBITS)) = RX_CRC_BITS;
  LRF_Interface_Generic_configOpRx(1, freq_hz, repeat && !_rx_long, len - RX_CRC_BITS / 8);
  _modem(&_modem_rx);
  LRF_programFrequency(freq_hz, false);
  LRF_enable();
  // Sync found: the MCE's event to SYSTIM channel 4 (capture), as TI's
  // RCL_Hal_setupSyncFoundCap(): the PBE's packet timestamps
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_SYSTIMEVTMUX0) = 21 << LRFDMDM_SYSTIMEVTMUX0_SEL0_S;
  HWREG_WRITE_LRF(LRFDDBELL_BASE + LRFDDBELL_O_SYSTIMOEV) = LRFDDBELL_SYSTIMOEV_SRC2_MCESYSTIM0;
  HWREG(SYSTIM_BASE + SYSTIM_O_CH4CFG) |=
      SYSTIM_CH4CFG_INP_RISE | SYSTIM_CH4CFG_MODE_CAPT | SYSTIM_CH4CFG_REARM_EN;
  _rx_fifo_size = LRF_prepareRxFifo();
  LRF_setRxFifoEffSz(_rx_long ? _rx_fifo_size - 4 : _rx_fifo_size);
  _rx_pos = _rx_got = 0;
  _rx_active = _rx_long;
  if (clear_stats) {
    _rx_ready = false;
    HWREGH_WRITE_LRF(BUFRAM(NRXOK)) = 0;
    HWREGH_WRITE_LRF(BUFRAM(NRXNOK)) = 0;
    HWREGH_WRITE_LRF(BUFRAM(NRXIGNORED)) = 0;
    HWREGH_WRITE_LRF(BUFRAM(NRXEMPTY)) = 0;
    HWREGH_WRITE_LRF(BUFRAM(NRXFIFOFULL)) = 0;
  }

  _clear_events(0xFFFFFFFFU);
  _start_at(_start_time(at, earliest, START_DELAY));
  LRF_waitForTopsmReady();
  LRF_Interface_Generic_sendOpRx();
  _running = true;
  return RF_OK;
}

int rf_rx_start(uint32_t freq_hz, uint16_t len, bool repeat, uint32_t at)
{
  return _rx_start(freq_hz, len, repeat, at, true);
}

bool rf_rx_read(rf_packet_t *pkt)
{
  if (_rx_long) {
    _rx_pump();
    if (!_rx_ready) return false;
    _rx_ready = false;
    memcpy(pkt->data, _rx_pkt.data, _rx_pkt.len);
    pkt->len = _rx_pkt.len;
    pkt->rssi = _rx_pkt.rssi;
    pkt->freq_est = _rx_pkt.freq_est;
    pkt->time = _rx_pkt.time;
    return true;
  }
  if (!LRF_hasRxWordToRead()) return false;
  uint32_t entry_len = LRF_peekRxFifo(0) & 0xFFFF;
  uint32_t words = (2 + entry_len + 3) / 4;
  uint32_t buf[(RF_FIFO_PAYLOAD + 16 + 3) / 4];
  bool ok = false;
  if (words <= sizeof(buf) / 4) {
    LRF_readRxFifoWords(buf, words);
    ok = _parse_entry((const uint8_t *)buf, words * 4, pkt);
  } else {
    LRF_discardRxFifoWords(words);
  }
  // Deallocated: the PBE may write there again
  LRF_setRxFifoEffSz(_rx_fifo_size);
  return ok;
}

uint16_t rf_rx_peek(uint8_t *buf, uint16_t max)
{
  if (!_running) return 0;
  uint32_t n;
  uint32_t w[(RF_FIFO_PAYLOAD + 16 + 3) / 4];
  const uint8_t *b = (const uint8_t *)w;
  if (_rx_long) {
    // Long: the copy so far
    _rx_pump();
    if (!_rx_active || _rx_ready) return 0;
    n = _rx_got;
    b = (const uint8_t *)_rx_buf;
  } else {
    if (LRF_hasRxWordToRead()) return 0;
    uint32_t rp;
    n = LRF_getUncommittedFifoStatus(&rp);
    if (n > sizeof(w)) n = sizeof(w);
    if (n < 4) return 0;
    LRF_peekRxFifoWords(w, (n + 3) / 4, rp);
  }
  // The entry's length field and numPad come first
  uint32_t off = 3U + b[2];
  if (n <= off) return 0;
  n -= off;
  memcpy(buf, b + off, n < max ? n : max);
  return n;
}

int rf_rx(uint32_t freq_hz, uint16_t len, uint32_t timeout_us, rf_packet_t *pkt)
{
  // One packet, then OPDONE
  int res = rf_rx_start(freq_hz, len < 2 ? 2 : len, false, 0);
  if (res) return res;
  uint32_t start = _now();

  // The PBE takes SYSTIM0 as the start; the stop is ours (hard stop)
  if (rf_debug) {
    for (int i = 0; i < 8; i++) {
      uint32_t t = _now();
      while (_now() - t < US(timeout_us / 8)) {
      }
      rf_debug(i, _events_now(), LRF_readRssi(), HWREGH_READ_LRF(LRFD_BUFRAM_BASE + 6));
    }
  }
  while (rf_busy() && (int32_t)(_now() - start - START_DELAY - US(timeout_us)) < 0) {
  }
  pkt->len = 0;
  if (rf_rx_read(pkt)) {
    rf_stop();
    return RF_OK;
  }
  res = rf_stop();
  return res == RF_ERR_TIMEOUT ? RF_ERR_NO_PACKET : res;
}

int8_t rf_rssi(void) { return _running ? LRF_readRssi() : RF_RSSI_NONE; }

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
