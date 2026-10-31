// Globals and helpers of Multiprotocol.ino (DIY-Multiprotocol-TX-Module,
// GPL v3)
#include <stdlib.h>

#include "board.h"
#include "chips.h"
#include "eeprom.h"

uint16_t counter;
uint8_t channel;
uint8_t packet[150];
uint8_t cyrfmfg_id[6];
uint8_t rx_id[5];
uint8_t phase;
uint16_t bind_counter;
uint8_t bind_phase;
uint8_t binding_idx;
uint16_t packet_period;
uint8_t packet_count;
uint8_t packet_sent;
uint8_t packet_length;
uint8_t hopping_frequency[78];
uint8_t* hopping_frequency_ptr;
uint8_t hopping_frequency_no;
uint8_t rf_ch_num;
uint8_t throttle, rudder, elevator, aileron;
uint8_t flags;
uint16_t crc;
uint16_t crc16_polynomial;
uint8_t crc8;
uint8_t crc8_polynomial;
uint16_t seed;
uint16_t failsafe_count;
uint16_t state;
uint8_t len;
uint8_t armed, arm_flags, arm_channel_previous;
uint8_t num_ch;
uint32_t pps_timer;
uint16_t pps_counter;
uint8_t calData[75];
uint8_t CH_AETR[16] = {AILERON, ELEVATOR, THROTTLE, RUDDER, CH5,  CH6,  CH7,  CH8,
                       CH9,     CH10,     CH11,     CH12,   CH13, CH14, CH15, CH16};
uint8_t CH_TAER[16] = {THROTTLE, AILERON, ELEVATOR, RUDDER, CH5,  CH6,  CH7,  CH8,
                       CH9,      CH10,    CH11,     CH12,   CH13, CH14, CH15, CH16};
uint8_t CH_EATR[16] = {ELEVATOR, AILERON, THROTTLE, RUDDER, CH5,  CH6,  CH7,  CH8,
                       CH9,      CH10,    CH11,     CH12,   CH13, CH14, CH15, CH16};
uint8_t prev_power = 0xFD;
uint8_t prev_option;
bool lbt_power;

uint8_t packet_in[TELEMETRY_BUFFER_SIZE];
uint8_t telemetry_in_buffer[TELEMETRY_BUFFER_SIZE];
uint8_t v_lipo1, v_lipo2;
uint8_t RX_RSSI, TX_RSSI, RX_LQI, TX_LQI;
uint8_t telemetry_link;
uint8_t telemetry_counter;
uint8_t telemetry_lost;
#ifdef SPORT_SEND
uint8_t SportData[MAX_SPORT_BUFFER];
uint8_t SportHead, SportTail;
#endif

uint8_t eeprom_read_byte(EE_ADDR address) { return eeprom_read(address); }

void eeprom_write_byte(EE_ADDR address, uint8_t value) { eeprom_write(address, value); }

// Called by the core before each protocol start: what protocol_init() resets
void mpm_protocol_prepare()
{
  crc16_polynomial = 0x1021;
  crc8_polynomial = 0x31;
  binding_idx = 0;
  prev_power = 0xFD;
  prev_option = option;
  lbt_power = false;
  // modules_reset()
#ifdef CC2500_INSTALLED
  CC2500_Reset();
#endif
#ifdef A7105_INSTALLED
  A7105_Reset();
#endif
#ifdef CYRF6936_INSTALLED
  CYRF_Reset();
#endif
  init_frskyd_link_telemetry();
  pps_timer = millis();
  pps_counter = 0;
}

// The core's idle hook: the chips' transactions go on between callbacks
void mpm_protocol_idle()
{
#ifdef CYRF6936_INSTALLED
  CYRF_Idle();
#endif
}

// The core's telemetry hook: Telemetry.ino's TelemetryUpdate() (Update_Telem())
void mpm_protocol_telemetry() { TelemetryUpdate(); }

// The core's frame hook: the protocol data of a radio frame (mpm.data), as
// update_serial_data() of Multiprotocol.ino (rx_len > 27)
void mpm_protocol_frame_data()
{
  const uint8_t* d = mpm.data;
  uint8_t n = mpm.data_len;
  (void)d;
  if (!n) return;
#if defined(FRSKYX_CC2500_INO)
  if ((protocol == PROTO_FRSKYX || protocol == PROTO_FRSKYX2) && n == 1) {
    // Bind options: telemetry off, channels 9-16
    if (binding_idx != d[0]) debugln("bind options %d", d[0]);
    binding_idx = d[0];
  }
#endif
#ifdef SPORT_SEND
#define BYTE_STUFF 0x7D
#define STUFF_MASK 0x20
  if ((protocol == PROTO_FRSKYX || protocol == PROTO_FRSKYX2) && n == 8) {
    // An S.Port frame for the receiver: physical id, then 7 bytes
    bool sport_valid = false;
    for (uint8_t i = 1; i < 8; i++)
      if (d[i] != 0) sport_valid = true;  // not a payload full of 0
    if ((d[0] & 0x1F) > 0x1B) sport_valid = false;
    if (sport_valid) {
      SportData[SportTail] = 0x7E;
      SportTail = (SportTail + 1) & (MAX_SPORT_BUFFER - 1);
      SportData[SportTail] = d[0] & 0x1F;
      SportTail = (SportTail + 1) & (MAX_SPORT_BUFFER - 1);
      for (uint8_t i = 1; i < 8; i++) {
        if (d[i] == BYTE_STUFF || d[i] == 0x7E) {
          SportData[SportTail] = BYTE_STUFF;
          SportTail = (SportTail + 1) & (MAX_SPORT_BUFFER - 1);
          SportData[SportTail] = d[i] ^ STUFF_MASK;
        } else
          SportData[SportTail] = d[i];
        SportTail = (SportTail + 1) & (MAX_SPORT_BUFFER - 1);
      }
      uint8_t used = SportTail;
      if (SportHead > SportTail)
        used += MAX_SPORT_BUFFER - SportHead;
      else
        used -= SportHead;
      if (used >= MAX_SPORT_BUFFER - (MAX_SPORT_BUFFER >> 2)) {
        // The status at once: the radio holds its frames
        DATA_BUFFER_LOW_on;
        SEND_MULTI_STATUS_on;
        debugln("Low buf=%d,h=%d,t=%d", used, SportHead, SportTail);
      }
    }
  }
#endif
}

// End_Bind() of the MPM for the protocols that count their bind packets
void end_bind_counter()
{
  if (bind_counter > 2) bind_counter = 2;
}

//
// Multiprotocol.ino
//

// random(max): xorshift32, seeded from the module id and the time of the
// first call
long mpm_random(long max)
{
  static uint32_t x;
  if (!x) x = (MProtocol_id ^ micros()) | 1;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  // As Arduino's: a positive value modulo max, max may be negative
  // (FrSky X: random(0xfefefefe))
  return max ? (long)(x >> 1) % max : 0;
}

void set_rx_tx_addr(uint32_t id)
{
  rx_tx_addr[0] = (id >> 24) & 0xFF;
  rx_tx_addr[1] = (id >> 16) & 0xFF;
  rx_tx_addr[2] = (id >> 8) & 0xFF;
  rx_tx_addr[3] = (id >> 0) & 0xFF;
  rx_tx_addr[4] = (rx_tx_addr[2] & 0xF0) | (rx_tx_addr[3] & 0x0F);
}

// Frequency hopping sequence in the range [02..77]
void calc_fh_channels(uint8_t num_ch)
{
  uint8_t idx = 0;
  uint32_t rnd = MProtocol_id;
  uint8_t max = (num_ch / 3) + 2;

  while (idx < num_ch) {
    uint8_t i;
    uint8_t count_2_26 = 0, count_27_50 = 0, count_51_74 = 0;

    rnd = rnd * 0x0019660D + 0x3C6EF35F;
    // Least significant byte. 73 is prime, so channels 76..77 are unused
    uint8_t next_ch = ((rnd >> 8) % 73) + 2;
    // A distance of 5 between consecutive channels
    if (idx != 0) {
      if (hopping_frequency[idx - 1] > next_ch) {
        if (hopping_frequency[idx - 1] - next_ch < 5) continue;
      } else if (next_ch - hopping_frequency[idx - 1] < 5) {
        continue;
      }
    }
    // Not duplicated, spread uniformly
    for (i = 0; i < idx; i++) {
      if (hopping_frequency[i] == next_ch) break;
      if (hopping_frequency[i] <= 26)
        count_2_26++;
      else if (hopping_frequency[i] <= 50)
        count_27_50++;
      else
        count_51_74++;
    }
    if (i != idx) continue;
    if ((next_ch <= 26 && count_2_26 < max) || (next_ch >= 27 && next_ch <= 50 && count_27_50 < max) ||
        (next_ch >= 51 && count_51_74 < max))
      hopping_frequency[idx++] = next_ch;
  }
}

uint8_t bit_reverse(uint8_t b_in)
{
  uint8_t b_out = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    b_out = (b_out << 1) | (b_in & 1);
    b_in >>= 1;
  }
  return b_out;
}

void crc16_update(uint8_t a, uint8_t bits)
{
  crc ^= a << 8;
  while (bits--)
    if (crc & 0x8000)
      crc = (crc << 1) ^ crc16_polynomial;
    else
      crc = crc << 1;
}

void crc8_update(uint8_t byte)
{
  crc8 = crc8 ^ byte;
  for (uint8_t j = 0; j < 8; j++)
    if (crc8 & 0x80)
      crc8 = (crc8 << 1) ^ crc8_polynomial;
    else
      crc8 <<= 1;
}

uint16_t rf_power_word(uint8_t db)
{
  static const struct {
    uint8_t db;
    uint16_t word;
  } steps[] = {RF_TX_POWER_STEPS};
  // The closest step, the lower power of two as close
  uint32_t best = 0;
  for (uint32_t i = 1; i < sizeof(steps) / sizeof(steps[0]); i++)
    if (abs(steps[i].db - db) <= abs(steps[best].db - db)) best = i;
  return steps[best].word;
}
