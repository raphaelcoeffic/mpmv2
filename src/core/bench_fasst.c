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

// FASST RX: parked on hop k, every packet despread (fasst_frame.h)
static radio_packet_t _pkt;
static uint8_t _bits[(FASST_RX_BITS_MAX + 7) / 8 + 1], _corr[FASST_RX_BITS_MAX];

static uint32_t _rx_bits(const fasst_rx_cfg_t* cfg)
{
  return FASST_RX_SYNC_LEFT + FASST_RX_EARLY_BITS + 8 * cfg->bytes;
}

// 8 bits from bit b, LSB first
static uint8_t _byte_at(uint32_t b) { return _bits[b / 8] >> (b % 8) | _bits[b / 8 + 1] << (8 - b % 8); }

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

static void _print_rx(fasst_print_t print, const fasst_rx_cfg_t* cfg)
{
  fasst_despread(_pkt.data, FASST_RX_SKIP_CHIPS, _rx_bits(cfg), _bits, _corr);
  print("@fasst %lu %u rssi=%d", _pkt.timestamp_us, cfg->k, _pkt.rssi);
  if (_pkt.freq_offset != RADIO_FREQ_OFFSET_NONE) print(" foff=%d", _pkt.freq_offset);

  // The rest of the sync word: next, or up to FASST_RX_EARLY_BITS later (the
  // RF core found its sync word in the preamble)
  uint16_t left = fasst_rx_sync_left(), mask = (1 << FASST_RX_SYNC_LEFT) - 1;
  uint32_t shift = 0;
  while (shift <= FASST_RX_EARLY_BITS &&
         ((_byte_at(shift) | (uint16_t)_byte_at(shift + 8) << 8) & mask) != left)
    shift++;
  if (shift > FASST_RX_EARLY_BITS) {
    print(" sync=none\r\n");
    return;
  }
  print(" shift=%lu ", shift);

  // The packet's bytes, then q: their worst bit correlation, one hex digit
  // each (11 = b)
  uint8_t v[FASST_RX_BITS_MAX / 8];
  uint32_t b0 = shift + FASST_RX_SYNC_LEFT;
  for (uint32_t i = 0; i < cfg->bytes; i++) v[i] = _byte_at(b0 + 8 * i);
  _print_hex_line(print, v, cfg->bytes, 2);
  print(" q=");
  for (uint32_t i = 0; i < cfg->bytes; i++) {
    v[i] = 11;
    for (int j = 0; j < 8; j++)
      if (_corr[b0 + 8 * i + j] < v[i]) v[i] = _corr[b0 + 8 * i + j];
  }
  _print_hex_line(print, v, cfg->bytes, 1);
  print("\r\n");
}

void fasst_rx_run(const fasst_rx_cfg_t* cfg, fasst_print_t print)
{
  radio_phy_t phy;
  fasst_phy(&phy, 0);
  phy.rx_bw = cfg->rx_bw;
  phy.sync_threshold = cfg->sync_threshold;
  radio_set_sync_word(0, fasst_rx_sync_word());
  uint16_t len = FASST_RX_LEN(_rx_bits(cfg));
  if (radio_init(&phy) != RADIO_OK || radio_set_frequency(fasst_freq_khz(cfg->k)) != RADIO_OK ||
      radio_rx_start(len) != RADIO_OK) {
    print("@err fasstrx radio status=0x%04x\r\n", radio_last_status());
    radio_off();
    return;
  }

  uint32_t packets = 0, restarts = 0, t_end = micros() + cfg->secs * 1000000;
  while (micros_before(t_end)) {
    if (radio_receive(&_pkt)) {
      packets++;
      _print_rx(print, cfg);
    } else if (!radio_rx_active()) {
      restarts++;
      radio_rx_start(len);
    }
  }
  radio_rx_stats_t st;
  radio_rx_stats(&st);
  radio_off();
  print("@end fasstrx packets=%lu restarts=%lu buf_full=%u\r\n", packets, restarts, st.buf_full);
}
