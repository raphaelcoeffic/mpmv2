#pragma once

// The MULTI-Module serial link with the radio (EdgeTX), as defined in
// Multiprotocol.h of the MPM: frames from the radio (100000 baud 8E2) and
// telemetry frames back ("MP" + type + length + data, MULTI_TELEMETRY)

#include <stdbool.h>
#include <stdint.h>

#define MPM_FRAME_V1_LEN 26  // channels only
#define MPM_FRAME_MAX_LEN 36  // V2: + flags byte + up to 9 bytes of protocol data
// The V2 flags byte (MPM_FRAME_V1_LEN): telemetry inverted, set by EdgeTX
// for the external bay, clear for an internal module
#define MPM_FRAME_TELEM_INVERT 0x08
#define MPM_FRAME_DATA_MAX 9
#define MPM_CHANNELS 16

// Channel values: 11 bits
#define MPM_CHANNEL_MIN 204     // -100%
#define MPM_CHANNEL_CENTER 1024
#define MPM_CHANNEL_MAX 1844    // +100%
// Failsafe values
#define MPM_FAILSAFE_NO_PULSE 0
#define MPM_FAILSAFE_HOLD 2047

typedef struct {
  bool failsafe;         // channels holds failsafe values instead
  uint8_t protocol;      // 0 to 255 (V1: 0 to 63), 0 = protocol list
  uint8_t sub_protocol;  // 0 to 7
  uint8_t rx_num;        // 0 to 63 (V1: 0 to 15)
  bool bind;
  bool autobind;
  bool range_check;
  bool low_power;
  int8_t option;
  uint16_t channels[MPM_CHANNELS];  // 0 to 2047
  // V2 (frames longer than 26 bytes)
  bool v2;
  bool telem_invert;
  bool disable_telem;
  bool disable_ch_map;
  uint8_t data[MPM_FRAME_DATA_MAX];  // protocol data (FrSky X S.Port, DSM...)
  uint8_t data_len;
} mpm_frame_t;

// Decodes a frame from the radio: false if it is not one (header, length)
bool mpm_frame_decode(const uint8_t* buf, uint8_t len, mpm_frame_t* f);

// Encodes a frame, as the radio sends it (V2 if f->v2): its length
uint8_t mpm_frame_encode(const mpm_frame_t* f, uint8_t* buf);

//
// Telemetry
//

#define MPM_TELEM_HDR_LEN 4
#define MPM_TELEM_STATUS 0x01
#define MPM_TELEM_PROTO_LIST 0x11

// Status flags
#define MPM_STATUS_INPUT 0x01           // input signal detected
#define MPM_STATUS_SERIAL 0x02          // serial mode
#define MPM_STATUS_PROTO_VALID 0x04
#define MPM_STATUS_BINDING 0x08
#define MPM_STATUS_WAIT_BIND 0x10       // waits for a bind event to load the protocol
#define MPM_STATUS_FAILSAFE 0x20        // the protocol supports failsafe
#define MPM_STATUS_CH_MAP 0x40          // the protocol supports disabling the channel mapping
#define MPM_STATUS_BUFFER_LOW 0x80

// Option field shown by the radio
typedef enum {
  MPM_OPTION_NONE = 0,
  MPM_OPTION_OPTION = 1,
  MPM_OPTION_RFTUNE = 2,
  MPM_OPTION_VIDFREQ = 3,
  MPM_OPTION_FIXEDID = 4,
  MPM_OPTION_TELEM = 5,
  MPM_OPTION_SRVFREQ = 6,
  MPM_OPTION_MAXTHR = 7,
  MPM_OPTION_RFCHAN = 8,
  MPM_OPTION_RFPOWER = 9,
  MPM_OPTION_WBUS = 10,
} mpm_option_type_t;

// Telemetry frame header: 4 bytes
uint8_t mpm_telem_header(uint8_t* buf, uint8_t type, uint8_t len);
