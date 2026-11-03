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
