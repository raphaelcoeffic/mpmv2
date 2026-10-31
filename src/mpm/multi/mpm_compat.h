#pragma once

// What the MULTI-Module protocol files expect from Multiprotocol.ino,
// Multiprotocol.h and TX_Def.h, on top of the mpm core: for the protocol
// files (Multiprotocol/*.ino as one C++ unit, mpm/sketch in the MULTI-Module
// repo; their C copies, mpm/upstream in mpm-v2) and the C files here. Names
// of the MPM, so no prefixes here

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../mpm.h"
#include "mpm_config.h"
#include "multi_enums.h"
#include "timer.h"

//
// Arduino / AVR
//
#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_byte_near(p) pgm_read_byte(p)
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define _BV(n) (1 << (n))
#define delayMicroseconds(us) delay_us(us)
#define delayMilliseconds(ms) delay_us((ms) * 1000UL)
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))  // Arduino
#endif
#define NOP() __asm__ __volatile__("nop")
// The MPM's DEBUG_SERIAL lines: the log (debug.h)
#include "debug.h"

//
// TX_Def.h: channels as the radio sends them (AETR)
//
#define NUM_CHN 16
#define CH1 0
#define CH2 1
#define CH3 2
#define CH4 3
#define CH5 4
#define CH6 5
#define CH7 6
#define CH8 7
#define CH9 8
#define CH10 9
#define CH11 10
#define CH12 11
#define CH13 12
#define CH14 13
#define CH15 14
#define CH16 15
#define AILERON 0
#define ELEVATOR 1
#define THROTTLE 2
#define RUDDER 3

#define CHANNEL_MAX_100 1844
#define CHANNEL_MIN_100 204
#define CHANNEL_MAX_125 2047
#define CHANNEL_MIN_125 0
#define CHANNEL_MID 1024
#define CHANNEL_MIN_COMMAND 409
#define CHANNEL_SWITCH 1106
#define CHANNEL_MAX_COMMAND 1639

#define FAILSAFE_CHANNEL_HOLD 2047
#define FAILSAFE_CHANNEL_NOPULSES 0

//
// The core's state under the MPM names
//
#define Channel_data mpm.channels
#define Failsafe_data mpm.failsafe
#define Channel_AUX mpm.channel_aux
#define protocol mpm.protocol
#define sub_protocol mpm.sub_protocol
#define RX_num mpm.rx_num
#define option ((uint8_t)mpm.option)
#define rx_tx_addr mpm.rx_tx_addr
#define MProtocol_id mpm.id
#define MProtocol_id_master mpm.id_master
#define option_override mpm.option_override

// Flags (Multiprotocol.h)
#define BIND_IN_PROGRESS (mpm.bind_in_progress = true)
#define BIND_DONE mpm_bind_done()
#define IS_BIND_IN_PROGRESS (mpm.bind_in_progress)
#define IS_BIND_DONE (!mpm.bind_in_progress)
#define IS_POWER_FLAG_on (!mpm.low_power)
#define IS_RANGE_FLAG_on (mpm.range_check)
#define IS_AUTOBIND_FLAG_on (mpm.autobind)
#define SUB_PROTO_INVALID mpm_sub_invalid()
#define IS_SUB_PROTO_VALID mpm_sub_valid()
#define IS_SUB_PROTO_INVALID (!mpm_sub_valid())
#define FAILSAFE_VALUES_off (mpm.failsafe_valid = false)
#define IS_FAILSAFE_VALUES_on (mpm.failsafe_valid)
#define IS_DISABLE_TELEM_on (mpm.disable_telem)
#define IS_DISABLE_TELEM_off (!mpm.disable_telem)
#define IS_DISABLE_CH_MAP_on (mpm.disable_ch_map)
#define IS_DISABLE_CH_MAP_off (!mpm.disable_ch_map)
#define IS_INPUT_SIGNAL_on (mpm.input)
#define IS_INPUT_SIGNAL_off (!mpm.input)
// FrSky X LBT: the next packet at LBT power (protocol_flags3)
extern bool lbt_power;
#define LBT_POWER_on (lbt_power = true)
#define LBT_POWER_off (lbt_power = false)
#define IS_LBT_POWER_on (lbt_power)
#define IS_LBT_POWER_off (!lbt_power)
// S.Port to the receiver: buffer filling up, in the status (protocol_flags3)
#define DATA_BUFFER_LOW_on (mpm.data_buffer_low = true)
#define DATA_BUFFER_LOW_off (mpm.data_buffer_low = false)
#define SEND_MULTI_STATUS_on mpm_send_status()

#define GET_FLAG(ch, mask) (ch ? mask : 0)
#define CH5_SW (Channel_AUX & _BV(0))
#define CH6_SW (Channel_AUX & _BV(1))
#define CH7_SW (Channel_AUX & _BV(2))
#define CH8_SW (Channel_AUX & _BV(3))
#define CH9_SW (Channel_AUX & _BV(4))
#define CH10_SW (Channel_AUX & _BV(5))
#define CH11_SW (Channel_AUX & _BV(6))
#define CH12_SW (Channel_AUX & _BV(7))
#define CH13_SW (Channel_data[CH13] > CHANNEL_SWITCH)
#define CH14_SW (Channel_data[CH14] > CHANNEL_SWITCH)
#define CH15_SW (Channel_data[CH15] > CHANNEL_SWITCH)
#define CH16_SW (Channel_data[CH16] > CHANNEL_SWITCH)

enum TXRX_State {
  TXRX_OFF,
  TX_EN,
  RX_EN
};

// NRF power (the XN297 emulation maps them to RF core power words)
enum NRF_POWER {
  NRF_POWER_0 = 0x00,  // -18dBm
  NRF_POWER_1 = 0x01,  // -12dBm
  NRF_POWER_2 = 0x02,  //  -6dBm
  NRF_POWER_3 = 0x03   //   0dBm
};
#define NRF_HIGH_POWER NRF_POWER_3
#define NRF_LOW_POWER NRF_POWER_1
#define NRF_RANGE_POWER NRF_POWER_0
#define NRF_BIND_POWER NRF_POWER_0

// CC2500 PATABLE values (cc2500.c maps them to RF core power words)
enum CC2500_POWER {
  CC2500_POWER_0 = 0x00,   // -55dbm or less
  CC2500_POWER_1 = 0x50,   // -30dbm
  CC2500_POWER_2 = 0x44,   // -28dbm
  CC2500_POWER_3 = 0xC0,   // -26dbm
  CC2500_POWER_4 = 0x84,   // -24dbm
  CC2500_POWER_5 = 0x81,   // -22dbm
  CC2500_POWER_6 = 0x46,   // -20dbm
  CC2500_POWER_7 = 0x93,   // -18dbm
  CC2500_POWER_8 = 0x55,   // -16dbm
  CC2500_POWER_9 = 0x8D,   // -14dbm
  CC2500_POWER_10 = 0xC6,  // -12dbm
  CC2500_POWER_11 = 0x97,  // -10dbm
  CC2500_POWER_12 = 0x6E,  //  -8dbm
  CC2500_POWER_13 = 0x7F,  //  -6dbm
  CC2500_POWER_14 = 0xA9,  //  -4dbm
  CC2500_POWER_15 = 0xBB,  //  -2dbm
  CC2500_POWER_16 = 0xFE,  //   0dbm
  CC2500_POWER_17 = 0xFF   //  +1dbm
};
#define CC2500_HIGH_POWER CC2500_POWER_17
#define CC2500_LBT_POWER CC2500_POWER_14
#define CC2500_LOW_POWER CC2500_POWER_13
#define CC2500_RANGE_POWER CC2500_POWER_1
#define CC2500_BIND_POWER CC2500_POWER_1

// A7105 TX_TEST values (a7105.c maps them to RF core power words)
enum A7105_POWER {
  A7105_POWER_0 = 0x00 << 3 | 0x00,  // -23dBm == PAC=0 TBG=0
  A7105_POWER_1 = 0x00 << 3 | 0x01,  // -20dBm == PAC=0 TBG=1
  A7105_POWER_2 = 0x00 << 3 | 0x02,  // -16dBm == PAC=0 TBG=2
  A7105_POWER_3 = 0x00 << 3 | 0x04,  // -11dBm == PAC=0 TBG=4
  A7105_POWER_4 = 0x01 << 3 | 0x05,  //  -6dBm == PAC=1 TBG=5
  A7105_POWER_5 = 0x02 << 3 | 0x07,  //   0dBm == PAC=2 TBG=7
  A7105_POWER_6 = 0x03 << 3 | 0x07,  //  +1dBm == PAC=3 TBG=7
  A7105_POWER_7 = 0x03 << 3 | 0x07   //  +1dBm == PAC=3 TBG=7
};
#define A7105_HIGH_POWER A7105_POWER_7
#define A7105_LOW_POWER A7105_POWER_3
#define A7105_RANGE_POWER A7105_POWER_0
#define A7105_BIND_POWER A7105_POWER_0

// CYRF6936 TX_CFG.PA_SETTING values (cyrf6936.c maps them to RF core power words)
enum CYRF_POWER {
  CYRF_POWER_0 = 0x00,  // -35dbm
  CYRF_POWER_1 = 0x01,  // -30dbm
  CYRF_POWER_2 = 0x02,  // -24dbm
  CYRF_POWER_3 = 0x03,  // -18dbm
  CYRF_POWER_4 = 0x04,  // -13dbm
  CYRF_POWER_5 = 0x05,  //  -5dbm
  CYRF_POWER_6 = 0x06,  //   0dbm
  CYRF_POWER_7 = 0x07   //  +4dbm
};
#define CYRF_HIGH_POWER CYRF_POWER_7
#define CYRF_LOW_POWER CYRF_POWER_3
#define CYRF_RANGE_POWER CYRF_POWER_1  // 1/30 of the full power distance
#define CYRF_BIND_POWER CYRF_POWER_0

//
// Globals of Multiprotocol.ino
//
extern uint16_t counter;
extern uint8_t channel;
extern uint8_t packet[150];
extern uint8_t cyrfmfg_id[6];
extern uint8_t rx_id[5];
extern uint8_t phase;
extern uint16_t bind_counter;
extern uint8_t bind_phase;
extern uint8_t binding_idx;
extern uint16_t packet_period;
extern uint8_t packet_count;
extern uint8_t packet_sent;
extern uint8_t packet_length;
extern uint8_t hopping_frequency[78];
extern uint8_t* hopping_frequency_ptr;
extern uint8_t hopping_frequency_no;
extern uint8_t rf_ch_num;
extern uint8_t throttle, rudder, elevator, aileron;
extern uint8_t flags;
extern uint16_t crc;
extern uint16_t crc16_polynomial;
extern uint8_t crc8;
extern uint8_t crc8_polynomial;
extern uint16_t seed;
extern uint16_t failsafe_count;
extern uint16_t state;
extern uint8_t len;
extern uint8_t armed, arm_flags, arm_channel_previous;
extern uint8_t num_ch;
extern uint32_t pps_timer;
extern uint16_t pps_counter;
extern uint8_t calData[75];
extern uint8_t CH_AETR[16], CH_TAER[16], CH_EATR[16];
extern uint8_t prev_power;
extern uint8_t prev_option;

// Telemetry (Multiprotocol.ino, Telemetry.ino: telemetry.c)
#define TELEMETRY_BUFFER_SIZE 32
extern uint8_t packet_in[TELEMETRY_BUFFER_SIZE];
extern uint8_t telemetry_in_buffer[TELEMETRY_BUFFER_SIZE];
extern uint8_t v_lipo1, v_lipo2;
extern uint8_t RX_RSSI, TX_RSSI, RX_LQI, TX_LQI;
extern uint8_t telemetry_link;
extern uint8_t telemetry_counter;
extern uint8_t telemetry_lost;
#ifdef SPORT_SEND
// S.Port frames of the radio for the receiver (FrSky X), stuffed
#define MAX_SPORT_BUFFER 64
extern uint8_t SportData[MAX_SPORT_BUFFER];
extern uint8_t SportHead, SportTail;
#endif

enum MultiPacketTypes {
  MULTI_TELEMETRY_STATUS = 1,
  MULTI_TELEMETRY_SPORT = 2,
  MULTI_TELEMETRY_HUB = 3,
  MULTI_TELEMETRY_DSM = 4,
  MULTI_TELEMETRY_DSMBIND = 5,
  MULTI_TELEMETRY_AFHDS2A = 6,
  MULTI_TELEMETRY_REUSE_1 = 7,
  MULTI_TELEMETRY_SYNC = 8,
  MULTI_TELEMETRY_REUSE_2 = 9,
  MULTI_TELEMETRY_HITEC = 10,
  MULTI_TELEMETRY_SCANNER = 11,
  MULTI_TELEMETRY_AFHDS2A_AC = 12,
  MULTI_TELEMETRY_RX_CHANNELS = 13,
  MULTI_TELEMETRY_HOTT = 14,
  MULTI_TELEMETRY_MLINK = 15,
  MULTI_TELEMETRY_CONFIG = 16,
  MULTI_TELEMETRY_PROTO = 17,
  MULTI_TELEMETRY_RLINK = 18,
};
#define boolean bool

void init_frskyd_link_telemetry();
bool frsky_process_telemetry(uint8_t* buffer, uint8_t len);
void frsky_link_frame();
void frsky_write_user_frame(uint8_t ID, uint8_t low, uint8_t high);
void frsky_send_user_frame(uint8_t ID, uint8_t low, uint8_t high);
void frsky_check_user_frame();
void TelemetryUpdate();

// FrSkyDVX_Common.ino state that Telemetry.ino uses
extern uint8_t FrSkyX_TX_Seq, FrSkyX_TX_IN_Seq, FrSkyX_RX_Seq;

//
// Convert.ino (with the protocol files), Multiprotocol.ino helpers (mpm_compat.c)
//
void reverse_channel(uint8_t num);
uint16_t convert_channel_ppm(uint8_t num);
uint16_t convert_channel_10b(uint8_t num, bool failsafe);
uint8_t convert_channel_8b(uint8_t num);
uint8_t convert_channel_8b_limit_deadband(uint8_t num, uint8_t min, uint8_t mid, uint8_t max,
                                          uint8_t deadband);
int16_t convert_channel_16b_limit(uint8_t num, int16_t min, int16_t max);
int16_t convert_channel_16b_nolimit(uint8_t num, int16_t min, int16_t max, bool failsafe);
uint8_t convert_channel_s8b(uint8_t num);
uint16_t limit_channel_100(uint8_t num);
void convert_channel_HK310(uint8_t num, uint8_t* low, uint8_t* high);
void convert_failsafe_HK310(uint8_t num, uint8_t* low, uint8_t* high);
uint16_t convert_channel_frsky(uint8_t num);

// FrSkyDVX_common.ino, for telemetry.c: init is a default argument in C++
#ifndef __cplusplus
uint16_t FrSkyX_crc(uint8_t* data, uint8_t len, uint16_t init);
#define FrSkyX_crc(...) _FRSKYX_CRC(__VA_ARGS__, 0, 0)
#define _FRSKYX_CRC(d, l, i, ...) FrSkyX_crc(d, l, i)
#endif

// EEPROM (Multiprotocol.h layout): eeprom.c, in the file system
typedef uint16_t EE_ADDR;
uint8_t eeprom_read_byte(EE_ADDR address);
void eeprom_write_byte(EE_ADDR address, uint8_t value);
#define AFHDS2A_EEPROM_OFFSET 50    // RX ID, 4 bytes per model id, end is 50+64=114
#define AFHDS2A_EEPROM_OFFSET2 250  // RX ID, 4 bytes per model id, end is 250+192=442
#define FRSKYD_CLONE_EEPROM_OFFSET 771
#define FRSKYX_CLONE_EEPROM_OFFSET 822
#define FRSKYX2_CLONE_EEPROM_OFFSET 873
#define DSM_CLONE_EEPROM_OFFSET 1074
#define EEPROM_CID_INIT_OFFSET 0  // 1 byte flag that Cyrf ID is initialized
#define EEPROM_CID_OFFSET 1       // 6 bytes Cyrf ID

// Arduino random(max)
long mpm_random(long max);
#define random(max) mpm_random(max)

void end_bind_counter();
void set_rx_tx_addr(uint32_t id);
void calc_fh_channels(uint8_t num_ch);
uint8_t bit_reverse(uint8_t b_in);
void crc16_update(uint8_t a, uint8_t bits);
void crc8_update(uint8_t byte);
