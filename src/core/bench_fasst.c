// FASST 10ch TX experiment (fasst_frame.h): one packet per 7 ms hop, with
// the given ID, hop step and PHY. Channels 200 + 170 i, ch1 swept; with
// fs_every, the F/S cycle (positions 1600 - 100 i) every fs_every frames.
// 7CH: one packet per 8 ms hop, channels 1320 (1520 us), ch1 swept 820..
// 1820, ch3 1720; with fs_every, ch3's F/S (1020) every fs_every packets
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
    ch[i] = cfg->ch7 ? 1320 : 200 + 170 * i;
    fs[i] = 1600 - 100 * i;
  }
  if (cfg->ch7) ch[2] = 1720;
  uint16_t ch_min = cfg->ch7 ? 820 : 200, ch_max = cfg->ch7 ? 1820 : 1800;
  uint32_t frame_us = cfg->ch7 ? FASST7_FRAME_US : FASST_FRAME_US;
  uint8_t pkt[FASST_PACKET_LEN];
  uint8_t k = cfg->step > FASST_FRANCE_STEP ? 1 : FASST_HOP_FIRST;
  uint32_t frames = cfg->secs * 1000000 / frame_us;
  uint32_t late = 0, errors = 0, fs_cycles = 0;
  int sweep_dir = 16;
  uint32_t t = micros() + 2000;
  for (uint32_t n = 0; n < frames; n++) {
    if (radio_set_frequency_start(fasst_freq_khz(k)) != RADIO_OK) errors++;

    if (cfg->sweep && n % cfg->sweep == 0) {
      if (ch[0] + sweep_dir > ch_max || ch[0] + sweep_dir < ch_min) sweep_dir = -sweep_dir;
      ch[0] += sweep_dir;
    }
    uint32_t len = FASST_CHIPS_LEN;
    uint8_t type = 0;
    if (cfg->ch7) {
      bool fs_packet = cfg->fs_every && n % cfg->fs_every == 0;
      fs_cycles += fs_packet;
      fasst7_packet(pkt, k, cfg->step, ch, fs_packet, cfg->fs_every ? 1020 : -1);
      fasst_spread_len(_chips, pkt, FASST7_PACKET_LEN);
      len = FASST7_CHIPS_LEN;
    } else if (cfg->fs_every && n % cfg->fs_every < FASST_FS_TYPES) {
      type = n % cfg->fs_every + 1;
      if (type == 1) fs_cycles++;
    }
    if (!cfg->ch7) {
      fasst_packet(pkt, k, cfg->step, ch, n & 1, type, fs, 0);
      fasst_spread(_chips, pkt);
    }
    if (cfg->invert)
      for (uint32_t i = 0; i < len; i++) _chips[i] ^= 0xFF;
    radio_set_sync_word(fasst_sync_word(_chips), 0);

    if (!micros_before(t - 200)) late++;
    if (radio_tx_start_at(fasst_chips(_chips), len - 4, t) != RADIO_OK) errors++;
    t += frame_us;
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

// _pkt despread into _bytes and _q (fasst_rx_bytes())
static int32_t _despread(const fasst_rx_cfg_t* cfg)
{
  return fasst_rx_bytes(_pkt.data, cfg->chips, cfg->bytes, _bytes, _q);
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

void fasstest_run(const fasstest_cfg_t* cfg, fasst_print_t print)
{
  radio_phy_t phy;
  fasst_phy(&phy, cfg->tx_power);
  if (radio_init(&phy) != RADIO_OK) {
    print("@err fasstest radio init status=0x%04x\r\n", radio_last_status());
    return;
  }
  uint8_t set = cfg->row >= 23, row = cfg->row % 23, code = fasstest_code(set, row);
  uint16_t ch[FASSTEST_TX_CHANNELS];
  for (int i = 0; i < FASSTEST_TX_CHANNELS; i++) ch[i] = 352 + 84 * i;
  uint8_t pkt[FASSTEST_PACKET_LEN];
  uint32_t frames = cfg->secs * 1000000 / FASSTEST_FRAME_US;
  uint32_t link_frames = cfg->link_secs * 1000000 / FASSTEST_FRAME_US;
  uint32_t late = 0, errors = 0, links = 0;
  int sweep_dir = 16;
  uint32_t t = micros() + 2000;
  // Link, as the T14SG: every FASSTEST_LINK_EVERY hops, the scan list's
  // channels FASSTEST_LINK_GAP hops apart, the hop after each listened to
  // for the receiver's reply (type 0x18 with our ID, ~14.9 ms after); a
  // reply ends the link, the data 2 hops after the link packet
  fasst_rx_cfg_t rx = {.bytes = FASSTEST_PACKET_LEN, .chips = FASST_BARKER_11};
  uint16_t rx_len = FASST_RX_LEN(rx.chips, _rx_bits(&rx));
  uint32_t replies = 0, heard = 0;
  uint8_t rx_id[4];
  memcpy(rx_id, cfg->rx, sizeof(rx_id));
  for (uint32_t n = 0; n < link_frames && !replies; n++, t += FASSTEST_FRAME_US) {
    uint32_t i = n % FASSTEST_LINK_EVERY;
    if (i % FASSTEST_LINK_GAP || i / FASSTEST_LINK_GAP >= FASSTEST_LINK_CHANNELS) continue;
    uint8_t lc = FASSTEST_LINK_CHANNEL(i / FASSTEST_LINK_GAP);
    while (micros_before(t - 2000)) {  // radio_tx_wait() gives up after 20 ms
    }
    if (radio_set_frequency_start(fasstest_freq_khz(lc)) != RADIO_OK) errors++;
    fasstest_link_packet(pkt, false, cfg->id, code, lc, FASSTEST_BFS_DEFAULT);
    fasst_spread_len(_chips, pkt, FASSTEST_PACKET_LEN);
    radio_set_sync_word(fasst_sync_word(_chips), fasst_rx_sync_word(rx.chips));
    if (!micros_before(t - 200)) late++;
    if (radio_tx_start_at(fasst_chips(_chips), FASSTEST_CHIPS_TX_LEN, t) != RADIO_OK) errors++;
    if (radio_tx_wait() != RADIO_OK) errors++;
    links++;
    radio_rx_start(rx_len);
    // The reply's sync about 15 ms after t, radio_receive() getting it
    // rx_us later: 3 ms spare
    while (micros_before(t + FASSTEST_LINK_REPLY_US + BIT_US(&rx, _rx_bits(&rx)) + 3000)) {
      if (radio_receive(&_pkt)) {
        int32_t shift = _despread(&rx);
        heard++;
        _print_rx(print, &rx, lc, shift);
        if (shift >= 0 && (_bytes[0] & 0x1C) == FASSTEST_TYPE_LINK_REPLY &&
            !memcmp(_bytes + 2, cfg->id, FASSTEST_ID_LEN)) {
          replies++;
          memcpy(rx_id, _bytes + 10, sizeof(rx_id));
          print("@link rx=%02x%02x%02x%02x\r\n", rx_id[0], rx_id[1], rx_id[2], rx_id[3]);
          break;
        }
      } else if (!radio_rx_active()) {
        radio_rx_start(rx_len);
      }
    }
    radio_rx_stop();
  }
  if (replies) t += FASSTEST_FRAME_US;
  // Data; a request: the next hop left out, its channel listened to
  uint32_t requests = 0, tele = 0, tele_bad = 0, rx_us = BIT_US(&rx, _rx_bits(&rx));
  uint8_t page = 0;
  for (uint32_t n = 0; n < frames; n++) {
    uint8_t c = fasstest_channel(set, row, n % FASSTEST_HOPS), req = 0;
    if (cfg->pages && n % FASSTEST_TELE_HOPS == 0 && n + 1 < frames) {
      while (!(cfg->pages >> page & 1)) page = (page + 1) % FASSTEST_PAGES;
      req = page + 1;
      page = (page + 1) % FASSTEST_PAGES;
    }
    if (radio_set_frequency_start(fasstest_freq_khz(c)) != RADIO_OK) errors++;
    if (cfg->sweep && n % cfg->sweep == 0) {
      if (ch[0] + sweep_dir > 1696 || ch[0] + sweep_dir < 352) sweep_dir = -sweep_dir;
      ch[0] += sweep_dir;
    }
    fasstest_packet(pkt, false, cfg->id, code, c, ch, n & 1, req);
    fasst_spread_len(_chips, pkt, FASSTEST_PACKET_LEN);
    radio_set_sync_word(fasst_sync_word(_chips), fasst_rx_sync_word(rx.chips));
    if (!micros_before(t - 200)) late++;
    if (radio_tx_start_at(fasst_chips(_chips), FASSTEST_CHIPS_TX_LEN, t) != RADIO_OK) errors++;
    if (req) {
      requests++;
      if (radio_tx_wait() != RADIO_OK) errors++;
      uint8_t rc = fasstest_channel(set, row, (n + 1) % FASSTEST_HOPS);
      if (radio_set_frequency(fasstest_freq_khz(rc)) != RADIO_OK) errors++;
      radio_rx_start(rx_len);
      bool got = false;
      while (!got && micros_before(t + FASSTEST_REPLY_US + rx_us + 3000)) {
        if (radio_receive(&_pkt)) {
          int32_t shift = _despread(&rx);
          uint8_t plain[18];
          got = shift >= 0 && (_bytes[0] & 0x1C) == FASSTEST_TYPE_REPLY &&
                fasstest_reply_unpack(_bytes, false, plain, rx_id, cfg->id, rc);
          if (got) {
            tele++;
            print("@tele %lu %u rssi=%d dt=%lu page=%u ", _pkt.timestamp_us, rc, _pkt.rssi,
                  _pkt.timestamp_us + BIT_US(&rx, shift) - t, plain[1] >> 4 & 3);
            _print_hex_line(print, plain + 2, 16, 2);
            print("\r\n");
          } else {
            tele_bad++;
            _print_rx(print, &rx, rc, shift);
          }
        } else if (!radio_rx_active()) {
          radio_rx_start(rx_len);
        }
      }
      radio_rx_stop();
      n++;  // the reply's hop
      t += FASSTEST_FRAME_US;
    }
    t += FASSTEST_FRAME_US;
  }
  if (radio_tx_wait() != RADIO_OK) errors++;
  radio_off();
  print("@ok fasstest links=%lu heard=%lu replies=%lu frames=%lu requests=%lu tele=%lu "
        "tele_bad=%lu late=%lu errors=%lu status=0x%04x\r\n",
        links, heard, replies, frames, requests, tele, tele_bad, late, errors,
        radio_last_status());
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

// Emulator: a packet's TX start this many bits before its RX timestamp +
// shift (the preamble and sync word, RF core latencies: the reply 15276 us
// after the request's TX start on the dongle's fasstest at 70, 14565 +
// 2 x 612 - 513)
#define FT_EM_TS_BITS 83

static void _em_tx(const uint8_t* p, uint8_t chips, uint8_t len, uint8_t c, uint32_t t,
                   uint32_t* late, uint32_t* errors)
{
  radio_rx_stop();
  if (radio_set_frequency(fasstest_freq_khz(c)) != RADIO_OK) (*errors)++;
  fasst_spread_n(_chips, chips, p, len);
  radio_set_sync_word(fasst_sync_word(_chips), fasst_rx_sync_word(chips));
  if (!micros_before(t - 200)) (*late)++;
  if (radio_tx_start_at(fasst_chips(_chips), (80 + 8 * len) * chips / 8 - 4, t) != RADIO_OK)
    (*errors)++;
  if (radio_tx_wait() != RADIO_OK) (*errors)++;
}

// Page p's data (bench_fasst.h), n replies sent before
static void _em_data(uint8_t* d, bool ch12, uint8_t page, uint32_t n)
{
  static const uint8_t page0[16] = {0xC0, 0x33, 0x7D, 0x80, 0xE8, 0x03, 0x80, 0x4A, 0x00, 0x78};
  memset(d, 0, 16);
  if (ch12) {
    d[0] = 0xC0;
    d[1] = 0x32;
    d[2] = n % 101;
  } else if (page == 0) {
    memcpy(d, page0, sizeof(page0));
    d[14] = n >> 8;
    d[15] = n;
  } else if (page == 1) {
    d[0] = 0x40;
    d[1] = 0x3C;
  }
}

void fasstest_em_run(const fasstest_em_cfg_t* cfg, fasst_print_t print)
{
  uint8_t id[FASSTEST_ID_LEN];
  memcpy(id, cfg->id, sizeof(id));
  bool linked = false, ch12 = cfg->ch12;
  for (int i = 0; i < FASSTEST_ID_LEN; i++) linked |= id[i] != 0;
  uint8_t park = linked ? cfg->park : FASSTEST_LINK_CHANNEL(0);
  fasst_rx_cfg_t rx = {.chips = linked && ch12 ? FASST_BARKER_7 : FASST_BARKER_11};
  rx.bytes = linked && ch12 ? FASSTEST_PACKET_LEN_12CH : FASSTEST_PACKET_LEN;
  radio_phy_t phy;
  fasst_phy(&phy, cfg->tx_power);
  radio_set_sync_word(0, fasst_rx_sync_word(rx.chips));
  uint16_t len = FASST_RX_LEN(rx.chips, _rx_bits(&rx));
  if (radio_init(&phy) != RADIO_OK || radio_set_frequency(fasstest_freq_khz(park)) != RADIO_OK ||
      radio_rx_start(len) != RADIO_OK) {
    print("@err ftrx radio status=0x%04x\r\n", radio_last_status());
    radio_off();
    return;
  }

  // Locked: on channel c, the next TX packet's sync (timestamp + shift) at
  // t_next on channel next, the radio moving there FASST_RX_LEAD_US before
  uint8_t c = park, next = 0, set = 0, row = 0;
  bool locked = false, hop_pending = false;
  uint32_t t_next = 0, misses = 0, rx_us = BIT_US(&rx, _rx_bits(&rx));
  uint32_t frame_us = ch12 ? FASSTEST_FRAME_US_12CH : FASSTEST_FRAME_US;
  uint32_t packets = 0, good = 0, links = 0, requests = 0, missed = 0, locks = 0, late = 0;
  uint32_t errors = 0;
  uint8_t pkt[FASSTEST_PACKET_LEN], data[16], plain[FASSTEST_PACKET_LEN];
  uint32_t t_end = micros() + cfg->secs * 1000000;
  while ((cfg->secs == 0 || micros_before(t_end)) && !(cfg->stop && cfg->stop())) {
    if (radio_receive(&_pkt)) {
      packets++;
      int32_t shift = _despread(&rx);
      if (shift < 0) continue;
      uint32_t t_sync = _pkt.timestamp_us + BIT_US(&rx, shift);
      uint32_t t_tx = t_sync - BIT_US(&rx, FT_EM_TS_BITS);
      if (!linked) {
        if (!fasstest_link_check(_bytes, c)) continue;
        // The link reply on its channel, then the data mode
        links++;
        fasstest_link_reply(pkt, _bytes, cfg->rx);
        _em_tx(pkt, FASST_BARKER_11, FASSTEST_PACKET_LEN, c, t_tx + FASSTEST_LINK_REPLY_US, &late,
               &errors);
        memcpy(id, _bytes + 2, FASSTEST_ID_LEN);
        ch12 = _bytes[14] & 0x02;
        fasstest_hop_code(_bytes, &set, &row);
        park = fasstest_channel(set, row, 0);
        print("@link id=%02x%02x%02x%02x%02x%02x code=%u ch12=%u bfs=%u\r\n", id[0], id[1],
              id[2], id[3], id[4], id[5], fasstest_code(set, row), ch12, _bytes[8]);
        linked = true;
        rx.chips = ch12 ? FASST_BARKER_7 : FASST_BARKER_11;
        rx.bytes = ch12 ? FASSTEST_PACKET_LEN_12CH : FASSTEST_PACKET_LEN;
        frame_us = ch12 ? FASSTEST_FRAME_US_12CH : FASSTEST_FRAME_US;
        rx_us = BIT_US(&rx, _rx_bits(&rx));
        len = FASST_RX_LEN(rx.chips, _rx_bits(&rx));
        radio_set_sync_word(0, fasst_rx_sync_word(rx.chips));
        c = park;
        radio_set_frequency(fasstest_freq_khz(c));
        radio_rx_start(len);
        continue;
      }
      // A TX packet with our ID: the hops followed, a request answered
      if ((_bytes[0] & 0x1C) != 0 || !fasstest_reply_unpack(_bytes, ch12, plain, id, id, c) ||
          !fasstest_hop_code(_bytes, &set, &row) || !fasstest_next_channel(set, row, c, &next))
        continue;
      good++;
      locks += !locked;
      locked = true;
      misses = 0;
      t_next = t_sync + frame_us;
      if (plain[1] & 0x20) {
        uint8_t page = ch12 ? 0 : plain[16] >> 1 & 3;
        _em_data(data, ch12, page, requests);
        fasstest_reply_packet(pkt, ch12, fasstest_code(set, row), page, data, cfg->rx, id, next);
        _em_tx(pkt, rx.chips, rx.bytes, next, t_tx + (ch12 ? FASSTEST_REPLY_US_12CH : FASSTEST_REPLY_US),
               &late, &errors);
        print("@req %lu %u page=%u reply=%u\r\n", t_sync, c, page, next);
        requests++;
        // The TX leaves the reply's hop out
        fasstest_next_channel(set, row, next, &next);
        t_next += frame_us;
      }
      hop_pending = true;
      if (!radio_rx_active()) radio_rx_start(len);
    } else if (hop_pending && !micros_before(t_next - FASST_RX_LEAD_US)) {
      radio_rx_stop();
      c = next;
      radio_set_frequency(fasstest_freq_khz(c));
      radio_rx_start(len);
      hop_pending = false;
    } else if (locked && !hop_pending && !micros_before(t_next + rx_us + FASST_RX_LATE_US)) {
      // No TX packet on this hop: the next one; FASSTEST_HOPS in a row: parked again
      missed++;
      if (++misses >= FASSTEST_HOPS) {
        print("@lost %lu\r\n", micros());
        locked = false;
        radio_rx_stop();
        c = park;
        radio_set_frequency(fasstest_freq_khz(c));
        radio_rx_start(len);
      } else {
        fasstest_next_channel(set, row, c, &next);
        t_next += frame_us;
        hop_pending = true;
      }
    } else if (!radio_rx_active()) {
      radio_rx_start(len);
    }
  }
  radio_off();
  print("@end ftrx packets=%lu good=%lu links=%lu replies=%lu missed=%lu locks=%lu late=%lu "
        "errors=%lu\r\n",
        packets, good, links, requests, missed, locks, late, errors);
}
