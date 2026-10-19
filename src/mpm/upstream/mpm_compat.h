#pragma once

// What the MULTI-Module protocol files expect from Multiprotocol.ino,
// Multiprotocol.h, TX_Def.h and Convert.ino (DIY-Multiprotocol-TX-Module
// v1.3.4.74, GPL v3), on top of the mpm core: the protocol files compile with
// an include added and few changes. Names of the MPM, so no prefixes here

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
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define _BV(n) (1 << (n))
#define delayMicroseconds(us) delay_us(us)
#define NOP() __asm__ __volatile__("nop")
#define debug(...) do {} while (0)
#define debugln(...) do {} while (0)

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

// Telemetry (not sent to the radio yet)
#define TELEMETRY_BUFFER_SIZE 32
extern uint8_t packet_in[TELEMETRY_BUFFER_SIZE];
extern uint8_t v_lipo1, v_lipo2;
extern uint8_t RX_RSSI, TX_RSSI, RX_LQI, TX_LQI;
extern uint8_t telemetry_link;
extern uint8_t telemetry_counter;
extern uint8_t telemetry_lost;

//
// Convert.ino, Multiprotocol.ino helpers
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

void end_bind_counter();
void set_rx_tx_addr(uint32_t id);
void calc_fh_channels(uint8_t num_ch);
uint8_t bit_reverse(uint8_t b_in);
void crc16_update(uint8_t a, uint8_t bits);
void crc8_update(uint8_t byte);
