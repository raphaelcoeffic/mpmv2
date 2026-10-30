// radio.h on the CC27xx (src/radio.c on the CC2652): rf.c's operations on
// phy_fsk's GFSK, for the radio bench (bench.c). The differences:
// - radio_phy_t's fsk: TX only (2-FSK), the RX is the same (rx_bw, if_freq
//   and sync_threshold are ignored: phy_fsk takes the bandwidth from the
//   deviation);
// - packets of up to RF_MAX_PAYLOAD bytes, the longer ones (above
//   RF_FIFO_PAYLOAD: CYRF6936 SDR) streamed through the PBE's FIFOs while
//   radio_tx_wait(), radio_tx_done(), radio_receive() or radio_rx_active()
//   poll: long RX restarts after each packet;
// - variable length RX receives max_len bytes (the frame decoders cut);
// - TX power words: the CC2652's (TI's table) taken as their dBm, words
//   below 0x100 as dBm (signed byte: "pwr 10", "pwr 0xf6" for -10);
// - extra overrides: (address, value) pairs of 32-bit register writes after
//   the setup;
// - a deviation large for the rate (redpineslow): RX at rx_os x the rate
//   (phy_fsk), each bit repeated rx_os times, a majority vote per bit (a
//   tie: the later half's first sample). The sync word's first 32 / rx_os
//   bits searched (its last ones would match d391d391's first half), the
//   rest received as data, checked (1 bit error at most) and dropped.

#include "radio.h"

#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_lrfdmdm.h)
#include DeviceFamily_constructPath(inc/hw_systim.h)

#include "phy_fsk.h"
#include "rf.h"

_Static_assert(RF_MAX_PAYLOAD <= RADIO_MAX_PKT_LEN, "rf.c packets in radio_packet_t");

// rf_packet_t's frequency estimate: steps of about deviation / 72
#define FREQ_EST_STEPS_PER_DEV 72
// Bluetooth LE 2M
#define BLE_2M_RATE 2000000
#define BLE_2M_DEVIATION 500000
// Preamble: MODPRECTRL's pattern size (8 or 16 bits) times its repetitions
// (1 to 16)
#define PREAMBLE_MAX_BYTES 32

typedef enum { STATE_IDLE, STATE_TX, STATE_RX } state_t;

static bool _init;
static radio_phy_t _phy;
static bool _phy_ok;
static uint32_t _freq_hz = 2440000000;
static uint32_t _tx_sync, _rx_sync, _rx_sync_alt;
static state_t _state;
static uint16_t _rx_len;
static uint16_t _rx_count;
static uint16_t _status;
static uint32_t _overrides[RADIO_EXTRA_OVERRIDES];
static uint8_t _n_overrides;
static uint8_t _rx_os = 1;
// rx_os > 1: the sync bytes received as data, their values (A, B)
static uint8_t _rx_sync_bytes;
static uint32_t _rx_sync_rest[2];
static uint16_t _rx_dropped;

// TI's CC2652 PA table (+5 dBm PA): power word, dBm
static const struct {
  uint16_t word;
  int8_t dbm;
} _power_words[] = {
    {0x04C6, -20}, {0x06C8, -18}, {0x06CA, -15}, {0x0ACC, -12}, {0x0ACF, -10},
    {0x0AD0, -9},  {0x10D4, -6},  {0x12D6, -5},  {0x1893, -3},  {0x2853, 0},
    {0x2856, 1},   {0x3259, 2},   {0x385D, 3},   {0x4E63, 4},   {0x7217, 5},
};

// Unknown words: the lowest (the bench's custom words lower the power)
static int8_t _dbm(uint16_t word)
{
  if (word < 0x100) return (int8_t)word;
  for (uint32_t i = 0; i < sizeof(_power_words) / sizeof(_power_words[0]); i++)
    if (_power_words[i].word == word) return _power_words[i].dbm;
  return _power_words[0].dbm;
}

static void _preamble(uint8_t bytes)
{
  if (bytes < 1) bytes = 1;
  if (bytes > PREAMBLE_MAX_BYTES) bytes = PREAMBLE_MAX_BYTES;
  uint32_t ctrl = bytes <= 16 ? (uint32_t)(bytes - 1) << LRFDMDM_MODPRECTRL_REPS_S |
                                    LRFDMDM_MODPRECTRL_SIZE_BITS8
                              : (uint32_t)((bytes + 1) / 2 - 1) << LRFDMDM_MODPRECTRL_REPS_S |
                                    LRFDMDM_MODPRECTRL_SIZE_BITS16;
  HWREG_WRITE_LRF(LRFDMDM_BASE + LRFDMDM_O_MODPRECTRL) = ctrl;
}

// micros() of a SYSTIM time in the past
static uint32_t _micros(uint32_t systim)
{
  uint32_t now = HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N);
  uint32_t us = HWREG(SYSTIM_BASE + SYSTIM_O_TIME1U);
  return us - (now - systim) / 4;
}

static radio_status_t _result(int res)
{
  _status = res;
  return res == RF_OK ? RADIO_OK : res == RF_ERR_PARAM ? RADIO_ERR_PARAM : RADIO_ERR_CMD;
}

// Ends a TX or RX
static void _stop()
{
  if (_state != STATE_IDLE) _status = rf_stop();
  _state = STATE_IDLE;
}

radio_status_t radio_init(const radio_phy_t* phy)
{
  if (!_init) {
    rf_init();
    _init = true;
  }
  _stop();
  _phy = *phy;
  phy_fsk_t fsk = {phy->symbol_rate, phy->deviation, -1, phy->fsk};
  if (phy->ble_2m) fsk = (phy_fsk_t){BLE_2M_RATE, BLE_2M_DEVIATION, -1, false};
  phy_fsk_info_t info;
  int res = phy_fsk_setup(&fsk, &info);
  _phy_ok = res == 0;
  _rx_os = _phy_ok ? info.rx_os : 1;
  if (!_phy_ok) {
    _status = res;
    return RADIO_ERR_BOOT;
  }
  _preamble(phy->ble_2m ? 2 : phy->preamble_bytes);
  for (uint8_t i = 0; i + 1 < _n_overrides; i += 2)
    *(volatile uint32_t*)_overrides[i] = _overrides[i + 1];
  rf_set_power(_dbm(phy->tx_power));
  rf_set_sync(_tx_sync, _phy.sync_bits);
  return RADIO_OK;
}

void radio_off()
{
  _stop();
}

radio_status_t radio_set_frequency(uint32_t freq_khz)
{
  _freq_hz = freq_khz * 1000;
  if (_state == STATE_RX) return radio_rx_start(_rx_len);
  return RADIO_OK;
}

void radio_set_sync_word(uint32_t tx_sync, uint32_t rx_sync)
{
  _tx_sync = tx_sync;
  _rx_sync = rx_sync;
}

void radio_set_rx_sync_alt(uint32_t rx_sync) { _rx_sync_alt = rx_sync; }

radio_status_t radio_tx_start_at(const uint8_t* data, uint16_t len, uint32_t at_us)
{
  if (!_phy_ok) return RADIO_ERR_PARAM;
  _stop();
  uint32_t at = 0;
  if (at_us) {
    int32_t ahead = (int32_t)(at_us - HWREG(SYSTIM_BASE + SYSTIM_O_TIME1U));
    if (ahead > 0) at = rf_time() + (uint32_t)ahead * 4;
  }
  rf_set_sync(_tx_sync, _phy.sync_bits);
  radio_status_t st = _result(rf_tx_start(_freq_hz, data, len, at));
  if (st == RADIO_OK) _state = STATE_TX;
  return st;
}

radio_status_t radio_tx_start(const uint8_t* data, uint16_t len)
{
  return radio_tx_start_at(data, len, 0);
}

bool radio_tx_done()
{
  return _state != STATE_TX || !rf_busy();
}

radio_status_t radio_tx_wait()
{
  if (_state != STATE_TX) return RADIO_OK;
  // The longest packet at the lowest rate (FrSky D, 255 bytes: 66 ms)
  uint32_t t0 = rf_time();
  while (rf_busy()) {
    if (rf_time() - t0 > 200000 * 4) break;
  }
  _state = STATE_IDLE;
  return _result(rf_stop());
}

radio_status_t radio_send(const uint8_t* data, uint16_t len)
{
  radio_status_t st = radio_tx_start(data, len);
  return st == RADIO_OK ? radio_tx_wait() : st;
}

// RX sync word at rx_os x the rate: its first bits, each repeated; the rest
// (whole bytes) in *rest
static uint32_t _os_sync(uint32_t sync, uint8_t bits, uint8_t *os_bits, uint32_t *rest)
{
  uint8_t rest_bits = bits * _rx_os > 32 ? (bits - 32 / _rx_os + 7) / 8 * 8 : 0;
  uint8_t n = bits - rest_bits;
  *rest = rest_bits ? sync & ((1U << rest_bits) - 1) : 0;
  sync >>= rest_bits;
  uint32_t out = 0;
  for (int i = n - 1; i >= 0; i--)
    for (uint8_t k = 0; k < _rx_os; k++) out = out << 1 | (sync >> i & 1);
  *os_bits = n * _rx_os;
  _rx_sync_bytes = rest_bits / 8;
  return out;
}

static void _rx_syncs(uint32_t a, uint32_t b)
{
  uint8_t bits;
  a = _os_sync(a, _phy.sync_bits, &bits, &_rx_sync_rest[0]);
  b = _os_sync(b, _phy.sync_bits, &bits, &_rx_sync_rest[1]);
  rf_set_syncs(a, b, bits);
}

// Received length at rx_os x the rate, the sync's rest included
// (RF_MAX_PAYLOAD at most)
static uint16_t _os_len(uint16_t len)
{
  uint32_t n = (uint32_t)(len + _rx_sync_bytes) * _rx_os;
  return n > RF_MAX_PAYLOAD ? RF_MAX_PAYLOAD / _rx_os * _rx_os : n;
}

static uint8_t _bit_errors(uint32_t a, uint32_t b)
{
  uint8_t n = 0;
  for (a ^= b; a; a &= a - 1) n++;
  return n;
}

// Majority vote per rx_os bits (MSB first), then the sync's rest checked
// and dropped. False: not the sync
static bool _vote(rf_packet_t *p)
{
  if (_rx_os == 1) return true;
  uint16_t n = p->len / _rx_os;
  uint8_t out[RF_MAX_PAYLOAD / 2] = {0};
  for (uint16_t i = 0; i < n * 8; i++) {
    uint8_t ones = 0, mid = 0;
    for (uint8_t k = 0; k < _rx_os; k++) {
      uint32_t j = (uint32_t)i * _rx_os + k;
      uint8_t bit = p->data[j / 8] >> (7 - j % 8) & 1;
      ones += bit;
      if (k == _rx_os / 2) mid = bit;
    }
    uint8_t bit = ones * 2 > _rx_os || (ones * 2 == _rx_os && mid);
    out[i / 8] |= bit << (7 - i % 8);
  }
  uint32_t rest = 0;
  for (uint8_t i = 0; i < _rx_sync_bytes && i < n; i++) rest = rest << 8 | out[i];
  if (n < _rx_sync_bytes || (_bit_errors(rest, _rx_sync_rest[0]) > 1 &&
                             _bit_errors(rest, _rx_sync_rest[1]) > 1)) {
    _rx_dropped++;
    return false;
  }
  p->len = n - _rx_sync_bytes;
  memcpy(p->data, out + _rx_sync_bytes, p->len);
  return true;
}

radio_status_t radio_rx_start(uint16_t len)
{
  if (!_phy_ok) return RADIO_ERR_PARAM;
  _stop();
  _rx_syncs(_rx_sync, _rx_sync_alt ? _rx_sync_alt : _rx_sync);
  radio_status_t st = _result(rf_rx_start(_freq_hz, _os_len(len), true, 0));
  if (st != RADIO_OK) return st;
  _state = STATE_RX;
  _rx_len = len;
  _rx_count = 0;
  _rx_dropped = 0;
  return RADIO_OK;
}

radio_status_t radio_rx_start_var(uint8_t hdr_len, uint16_t max_len, radio_len_fn_t len_fn,
                                  void* ctx)
{
  (void)hdr_len, (void)len_fn, (void)ctx;
  return radio_rx_start(max_len);
}

static void _packet(const rf_packet_t* p, radio_packet_t* pkt, bool freq_offset)
{
  memcpy(pkt->data, p->data, p->len);
  pkt->len = p->len;
  pkt->rssi = p->rssi;
  pkt->timestamp_us = _micros(p->time);
  uint32_t dev = _phy.ble_2m ? BLE_2M_DEVIATION : _phy.deviation;
  pkt->freq_offset = freq_offset ? (int16_t)((int32_t)p->freq_est * (int32_t)dev /
                                             (FREQ_EST_STEPS_PER_DEV * 1000))
                                 : RADIO_FREQ_OFFSET_NONE;
}

radio_status_t radio_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                             uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt)
{
  pkt->len = 0;
  radio_status_t st = radio_send(data, len);
  if (st != RADIO_OK) return st;
  _rx_syncs(rx_sync, rx_sync);
  rf_packet_t p;
  int res = rf_rx(_freq_hz, _os_len(rx_len), window_us, &p);
  if (res == RF_ERR_NO_PACKET) return RADIO_NO_PACKET;
  if (res != RF_OK) return _result(res);
  if (!_vote(&p)) return RADIO_NO_PACKET;
  _packet(&p, pkt, false);
  return RADIO_OK;
}

void radio_rx_stop()
{
  if (_state == STATE_RX) _stop();
}

bool radio_rx_active()
{
  return _state == STATE_RX && rf_busy();
}

radio_status_t radio_set_tx_power(uint16_t tx_power)
{
  _phy.tx_power = tx_power;
  rf_set_power(_dbm(tx_power));
  return RADIO_OK;
}

int8_t radio_rssi()
{
  if (_state != STATE_RX) return RADIO_RSSI_NONE;
  int8_t rssi = rf_rssi();
  return rssi == RF_RSSI_NONE ? RADIO_RSSI_NONE : rssi;
}

// The PBE's CRC verdicts are on the last byte only (rf.c): all packets
// count as ok, nok the ones whose sync's rest is wrong (rx_os > 1)
void radio_rx_stats(radio_rx_stats_t* stats)
{
  rf_stats_t st;
  rf_stats(&st);
  stats->ok = _rx_count;
  stats->nok = _rx_dropped;
  stats->buf_full = st.rx_fifo_full;
}

bool radio_receive(radio_packet_t* pkt)
{
  if (_state != STATE_RX) return false;
  rf_packet_t p;
  if (!rf_rx_read(&p) || !_vote(&p)) return false;
  _rx_count++;
  _packet(&p, pkt, true);
  return true;
}

void radio_set_extra_overrides(const uint32_t* words, uint8_t n)
{
  if (n > RADIO_EXTRA_OVERRIDES) n = RADIO_EXTRA_OVERRIDES;
  memcpy(_overrides, words, n * sizeof(words[0]));
  _n_overrides = n;
}

uint16_t radio_last_status() { return _status; }

int16_t radio_hposc_offset() { return 0; }
