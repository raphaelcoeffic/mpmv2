// Globals and helpers of Multiprotocol.ino and Convert.ino
// (DIY-Multiprotocol-TX-Module v1.3.4.74, GPL v3)
#include "mpm_compat.h"

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

uint8_t packet_in[TELEMETRY_BUFFER_SIZE];
uint8_t v_lipo1, v_lipo2;
uint8_t RX_RSSI, TX_RSSI, RX_LQI, TX_LQI;
uint8_t telemetry_link;
uint8_t telemetry_counter;
uint8_t telemetry_lost;

// Called by the core before each protocol start: what protocol_init() resets
void mpm_protocol_prepare()
{
  crc16_polynomial = 0x1021;
  crc8_polynomial = 0x31;
  binding_idx = 0;
  prev_power = 0xFD;
  // init_frskyd_link_telemetry()
  telemetry_link = 0;
  telemetry_lost = 1;
  telemetry_counter = 0;
  v_lipo1 = v_lipo2 = 0;
  RX_RSSI = TX_RSSI = RX_LQI = TX_LQI = 0;
  pps_timer = millis();
  pps_counter = 0;
}

// End_Bind() of the MPM for the protocols that count their bind packets
void end_bind_counter()
{
  if (bind_counter > 2) bind_counter = 2;
}

//
// Convert.ino
//

void reverse_channel(uint8_t num)
{
  uint16_t val = 2048 - Channel_data[num];
  if (val >= 2048) val = 2047;
  Channel_data[num] = val;
}

uint16_t convert_channel_ppm(uint8_t num)
{
  uint16_t val = Channel_data[num];
  return (((val << 2) + val) >> 3) + 860;
}

uint16_t convert_channel_10b(uint8_t num, bool failsafe)
{
  uint16_t val = failsafe ? Failsafe_data[num] : Channel_data[num];
  val = ((val << 2) + val) >> 3;
  if (val <= 128) return 0;
  if (val >= 1152) return 1023;
  return val - 128;
}

uint8_t convert_channel_8b(uint8_t num)
{
  uint16_t val = Channel_data[num];
  val = ((val << 2) + val) >> 5;
  if (val <= 32) return 0;
  if (val >= 288) return 255;
  return val - 32;
}

uint8_t convert_channel_8b_limit_deadband(uint8_t num, uint8_t min, uint8_t mid, uint8_t max,
                                          uint8_t deadband)
{
  uint16_t val = limit_channel_100(num);
  uint16_t db_low = CHANNEL_MID - deadband, db_high = CHANNEL_MID + deadband;
  int32_t calc;
  uint8_t out;
  if (val >= db_low && val <= db_high) return mid;
  if (val < db_low) {
    val -= CHANNEL_MIN_100;
    calc = mid - min;
    calc *= val;
    calc /= (db_low - CHANNEL_MIN_100);
    out = calc;
    out += min;
  } else {
    val -= db_high;
    calc = max - mid;
    calc *= val;
    calc /= (CHANNEL_MAX_100 - db_high + 1);
    out = calc;
    out += mid;
    if (max > min)
      out++;
    else
      out--;
  }
  return out;
}

int16_t convert_channel_16b_limit(uint8_t num, int16_t min, int16_t max)
{
  int32_t val = limit_channel_100(num);
  val = (val - CHANNEL_MIN_100) * (max - min) / (CHANNEL_MAX_100 - CHANNEL_MIN_100) + min;
  return (uint16_t)val;
}

int16_t convert_channel_16b_nolimit(uint8_t num, int16_t min, int16_t max, bool failsafe)
{
  int32_t val = failsafe ? Failsafe_data[num] : Channel_data[num];
  val = (val - CHANNEL_MIN_100) * (max - min) / (CHANNEL_MAX_100 - CHANNEL_MIN_100) + min;
  return (uint16_t)val;
}

uint8_t convert_channel_s8b(uint8_t num)
{
  uint8_t ch = convert_channel_8b(num);
  return (ch < 128 ? 127 - ch : ch);
}

uint16_t limit_channel_100(uint8_t num)
{
  if (Channel_data[num] >= CHANNEL_MAX_100) return CHANNEL_MAX_100;
  if (Channel_data[num] <= CHANNEL_MIN_100) return CHANNEL_MIN_100;
  return Channel_data[num];
}

void convert_channel_HK310(uint8_t num, uint8_t* low, uint8_t* high)
{
  uint16_t temp = 0xFFFF - (3440 + ((Channel_data[num] * 5) >> 1)) / 3;
  *low = (uint8_t)(temp & 0xFF);
  *high = (uint8_t)(temp >> 8);
}

void convert_failsafe_HK310(uint8_t num, uint8_t* low, uint8_t* high)
{
  uint16_t temp = 0xFFFF - (3440 + ((Failsafe_data[num] * 5) >> 1)) / 3;
  *low = (uint8_t)(temp & 0xFF);
  *high = (uint8_t)(temp >> 8);
}

uint16_t convert_channel_frsky(uint8_t num)
{
  uint16_t val = Channel_data[num];
  return ((val * 15) >> 4) + 1290;
}

//
// Multiprotocol.ino
//

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
