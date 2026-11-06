// FASST 10ch TX experiment (fasst_frame.h): one packet per 7 ms hop, with
// the given ID, hop step and PHY. Channels 200 + 170 i, ch1 swept; with
// fs_every, the F/S cycle (positions 1600 - 100 i) every fs_every frames
#include <string.h>

#include "bench_fasst.h"
#include "fasst_frame.h"
#include "radio.h"
#include "timer.h"

static uint8_t _chips[FASST_CHIPS_LEN];

void fasst_run(const fasst_cfg_t* cfg, fasst_print_t print)
{
  radio_phy_t phy;
  fasst_phy(&phy, cfg->tx_power);
  phy.symbol_rate = cfg->chip_rate;
  phy.deviation = cfg->deviation;
  if (radio_init(&phy) != RADIO_OK) {
    print("@err fasst radio init status=0x%04x\r\n", radio_last_status());
    return;
  }

  fasst_set_id(cfg->id);
  uint16_t ch[FASST_CHANNELS], fs[FASST_CHANNELS];
  for (int i = 0; i < FASST_CHANNELS; i++) {
    ch[i] = 200 + 170 * i;
    fs[i] = 1600 - 100 * i;
  }
  uint8_t pkt[FASST_PACKET_LEN];
  uint8_t k = FASST_HOP_FIRST;
  uint32_t frames = cfg->secs * 1000000 / FASST_FRAME_US;
  uint32_t late = 0, errors = 0, fs_cycles = 0;
  int sweep_dir = 16;
  uint32_t t = micros() + 2000;
  for (uint32_t n = 0; n < frames; n++) {
    if (radio_set_frequency_start(fasst_freq_khz(k)) != RADIO_OK) errors++;

    if (cfg->sweep && n % cfg->sweep == 0) {
      if (ch[0] + sweep_dir > 1800 || ch[0] + sweep_dir < 200) sweep_dir = -sweep_dir;
      ch[0] += sweep_dir;
    }
    uint8_t type = 0;
    if (cfg->fs_every && n % cfg->fs_every < FASST_FS_TYPES) {
      type = n % cfg->fs_every + 1;
      if (type == 1) fs_cycles++;
    }
    fasst_packet(pkt, k, cfg->step, ch, n & 1, type, fs, 0);
    fasst_spread(_chips, pkt);
    if (cfg->invert)
      for (int i = 0; i < FASST_CHIPS_LEN; i++) _chips[i] ^= 0xFF;
    radio_set_sync_word(fasst_sync_word(_chips), 0);

    if (!micros_before(t - 200)) late++;
    if (radio_tx_start_at(fasst_chips(_chips), FASST_CHIPS_TX_LEN, t) != RADIO_OK) errors++;
    t += FASST_FRAME_US;
    k = fasst_next_hop(k, cfg->step);
  }
  if (radio_tx_wait() != RADIO_OK) errors++;
  radio_off();
  fasst_set_id(FASST_ID_DEFAULT);
  print("@ok fasst frames=%lu late=%lu errors=%lu fs_cycles=%lu status=0x%04x\r\n", frames, late,
        errors, fs_cycles, radio_last_status());
}

// FASST RX: parked on hop k or following the hops, every packet despread
// (fasst_frame.h)
static radio_packet_t _pkt;
static uint8_t _bits[(FASST_RX_BITS_MAX + 7) / 8 + 1], _corr[FASST_RX_BITS_MAX];
// The packet's bytes after the sync word and their worst bit correlation
static uint8_t _bytes[FASST_RX_BYTES_MAX], _q[FASST_RX_BYTES_MAX];

// n bits at 1.5 Mchip/s, us
#define BIT_US(cfg, n) ((n) * (cfg)->chips * 2 / 3)

// Following the hops: RX on FASST_RX_LEAD_US before the next packet's sync
// word (its preamble starts about 500 us before), the packet given up
// FASST_RX_LATE_US after its RX would have ended
#define FASST_RX_LEAD_US 1000
#define FASST_RX_LATE_US 500

static uint32_t _rx_bits(const fasst_rx_cfg_t* cfg)
{
  return FASST_RX_SYNC_LEFT(cfg->chips) + FASST_RX_EARLY_BITS + 8 * cfg->bytes;
}

// 8 bits from bit b, LSB first
static uint8_t _byte_at(uint32_t b) { return _bits[b / 8] >> (b % 8) | _bits[b / 8 + 1] << (8 - b % 8); }

// _pkt despread into _bytes and _q: the bits from the RF core's sync word to
// the real one (the sync bits left: next, or up to FASST_RX_EARLY_BITS later
// when the RF core found its sync word in the preamble), -1 if not found
static int32_t _despread(const fasst_rx_cfg_t* cfg)
{
  uint8_t n = cfg->chips;
  fasst_despread(_pkt.data, n, FASST_RX_SKIP_CHIPS(n), _rx_bits(cfg), _bits, _corr);
  uint16_t left = fasst_rx_sync_left(n), mask = (1 << FASST_RX_SYNC_LEFT(n)) - 1;
  uint32_t shift = 0;
  while (shift <= FASST_RX_EARLY_BITS &&
         ((_byte_at(shift) | (uint16_t)_byte_at(shift + 8) << 8) & mask) != left)
    shift++;
  if (shift > FASST_RX_EARLY_BITS) return -1;

  uint32_t b0 = shift + FASST_RX_SYNC_LEFT(n);
  for (uint32_t i = 0; i < cfg->bytes; i++) {
    _bytes[i] = _byte_at(b0 + 8 * i);
    _q[i] = n;
    for (int j = 0; j < 8; j++)
      if (_corr[b0 + 8 * i + j] < _q[i]) _q[i] = _corr[b0 + 8 * i + j];
  }
  return shift;
}

// v[0..n) in hex, 2 digits each, or 1 (values 0..15)
static void _print_hex_line(fasst_print_t print, const uint8_t* v, uint32_t n, int digits)
{
  static const char hex[] = "0123456789abcdef";
  char line[2 * 32 + 1];
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (digits == 2) line[k++] = hex[v[i] >> 4];
    line[k++] = hex[v[i] & 0xF];
    if (k >= sizeof(line) - 2 || i == n - 1) {
      line[k] = 0;
      print("%s", line);
      k = 0;
    }
  }
}

// _pkt, heard on hop k, after _despread()
static void _print_rx(fasst_print_t print, const fasst_rx_cfg_t* cfg, uint8_t k, int32_t shift)
{
  print("@fasst %lu %u rssi=%d", _pkt.timestamp_us, k, _pkt.rssi);
  if (_pkt.freq_offset != RADIO_FREQ_OFFSET_NONE) print(" foff=%d", _pkt.freq_offset);
  if (shift < 0) {
    print(" sync=none\r\n");
    return;
  }
  print(" shift=%ld ", shift);
  _print_hex_line(print, _bytes, cfg->bytes, 2);
  print(" q=");
  _print_hex_line(print, _q, cfg->bytes, 1);
  print("\r\n");
}

// A FASSTest packet good enough to follow: every bit of its first len
// bytes at most one chip off
static bool _fasstest_clean(const fasst_rx_cfg_t* cfg, uint16_t len)
{
  for (int i = 0; i < len; i++)
    if (_q[i] < cfg->chips - 2) return false;
  return true;
}

// FASSTest: parked on channel cfg->k or following the hops (bench_fasst.h).
// Locked: on channel c, the next TX packet's sync word at t_next on channel
// next, the radio moving there FASST_RX_LEAD_US before (hop_pending)
static void _fasstest_rx_run(const fasst_rx_cfg_t* cfg, fasst_print_t print, uint16_t len)
{
  uint8_t c = cfg->k, next = 0, set = 0, row = 0;
  bool locked = false, hop_pending = false;
  uint32_t t_next = 0, misses = 0, rx_us = BIT_US(cfg, _rx_bits(cfg));
  uint32_t packets = 0, clean = 0, replies = 0, missed = 0, locks = 0, restarts = 0;
  bool ch12 = cfg->chips == FASST_BARKER_7;
  uint16_t plen = ch12 ? FASSTEST_PACKET_LEN_12CH : FASSTEST_PACKET_LEN;
  uint32_t frame_us = ch12 ? FASSTEST_FRAME_US_12CH : FASSTEST_FRAME_US;
  uint32_t t_end = micros() + cfg->secs * 1000000;
  while ((cfg->secs == 0 || micros_before(t_end)) && !(cfg->stop && cfg->stop())) {
    if (radio_receive(&_pkt)) {
      packets++;
      int32_t shift = _despread(cfg);
      bool good = shift >= 0 && cfg->bytes >= plen && _fasstest_clean(cfg, plen);
      clean += good;
      if (good && (_bytes[0] & 0x1C) == FASSTEST_TYPE_REPLY) {  // 0x10, 0x13 (12CH)
        replies++;
      } else if (good && cfg->follow && fasstest_hop_code(_bytes, &set, &row) &&
                 fasstest_next_channel(set, row, c, &next)) {
        locks += !locked;
        locked = true;
        misses = 0;
        t_next = _pkt.timestamp_us + BIT_US(cfg, shift) + frame_us;
        hop_pending = true;
      }
      _print_rx(print, cfg, c, shift);
    } else if (hop_pending && !micros_before(t_next - FASST_RX_LEAD_US)) {
      radio_rx_stop();
      c = next;
      radio_set_frequency(fasstest_freq_khz(c));
      radio_rx_start(len);
      hop_pending = false;
    } else if (locked && !hop_pending && !micros_before(t_next + rx_us + FASST_RX_LATE_US)) {
      // No TX packet on this hop: the next one, FASSTEST_HOPS in a row: parked again
      missed++;
      if (++misses >= FASSTEST_HOPS) {
        locked = false;
        radio_rx_stop();
        c = cfg->k;
        radio_set_frequency(fasstest_freq_khz(c));
        radio_rx_start(len);
      } else {
        fasstest_next_channel(set, row, c, &next);
        t_next += frame_us;
        hop_pending = true;
      }
    } else if (!radio_rx_active()) {
      restarts++;
      radio_rx_start(len);
    }
  }
  radio_rx_stats_t st;
  radio_rx_stats(&st);
  radio_off();
  print("@end fasstrx packets=%lu clean=%lu replies=%lu missed=%lu locks=%lu restarts=%lu "
        "buf_full=%u\r\n",
        packets, clean, replies, missed, locks, restarts, st.buf_full);
}

void fasst_rx_run(const fasst_rx_cfg_t* cfg, fasst_print_t print)
{
  radio_phy_t phy;
  fasst_phy(&phy, 0);
  phy.rx_bw = cfg->rx_bw;
  phy.sync_threshold = cfg->sync_threshold;
  radio_set_sync_word(0, fasst_rx_sync_word(cfg->chips));
  uint16_t len = FASST_RX_LEN(cfg->chips, _rx_bits(cfg));
  uint32_t freq = cfg->fasstest ? fasstest_freq_khz(cfg->k) : fasst_freq_khz(cfg->k);
  if (radio_init(&phy) != RADIO_OK || radio_set_frequency(freq) != RADIO_OK ||
      radio_rx_start(len) != RADIO_OK) {
    print("@err fasstrx radio status=0x%04x\r\n", radio_last_status());
    radio_off();
    return;
  }
  if (cfg->fasstest) {
    _fasstest_rx_run(cfg, print, len);
    return;
  }

  // Locked: on hop k, the next packet's sync word at t_next. A packet with a
  // good CRC (re)locks: its hop index and step give the next hop. A miss
  // keeps the timing, one hop further; FASST_HOPS in a row: parked again
  uint8_t k = cfg->k, step = 0;
  bool locked = false, listening = true;
  uint32_t t_next = 0, misses = 0, rx_us = BIT_US(cfg, _rx_bits(cfg));
  uint32_t packets = 0, good = 0, missed = 0, locks = 0, restarts = 0;
  uint32_t t_end = micros() + cfg->secs * 1000000;
  while ((cfg->secs == 0 || micros_before(t_end)) && !(cfg->stop && cfg->stop())) {
    if (radio_receive(&_pkt)) {
      packets++;
      int32_t shift = _despread(cfg);
      uint8_t plain[FASST_PACKET_LEN], pk, pstep, heard = k;
      bool ok = shift >= 0 && cfg->bytes >= FASST_PACKET_LEN && fasst_unpack(_bytes, plain, &pk, &pstep);
      good += ok;
      if (cfg->follow && ok) {
        locks += !locked;
        locked = true;
        misses = 0;
        step = pstep;
        k = fasst_next_hop(pk, step);
        t_next = _pkt.timestamp_us + BIT_US(cfg, shift) + FASST_FRAME_US;
        radio_set_frequency(fasst_freq_khz(k));
        listening = false;
      }
      _print_rx(print, cfg, heard, shift);
    } else if (locked && !listening && !micros_before(t_next - FASST_RX_LEAD_US)) {
      radio_rx_start(len);
      listening = true;
    } else if (locked && listening && !micros_before(t_next + rx_us + FASST_RX_LATE_US)) {
      missed++;
      k = fasst_next_hop(k, step);
      t_next += FASST_FRAME_US;
      if (++misses >= FASST_HOPS) {
        locked = false;
        k = cfg->k;
      }
      radio_set_frequency(fasst_freq_khz(k));
      listening = !locked;
      if (listening) radio_rx_start(len);
    } else if (listening && !radio_rx_active()) {
      restarts++;
      radio_rx_start(len);
    }
  }
  radio_rx_stats_t st;
  radio_rx_stats(&st);
  radio_off();
  print("@end fasstrx packets=%lu crc_ok=%lu missed=%lu locks=%lu restarts=%lu buf_full=%u\r\n",
        packets, good, missed, locks, restarts, st.buf_full);
}
