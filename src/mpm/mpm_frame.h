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
// Futaba receiver telemetry, all systems (FASSTest: proto_fasst.c;
// T-FHSS Surface and Air: TFHSS_cc2500.c): their replies carry S.BUS2 slot
// data as the sensors put it on the wire. Not assigned upstream yet (the
// last there: RLink, 18; this would be MULTI_TELEMETRY_FUTABA, 19).
// Byte 0: the first slot (0..31), byte 1: the reply's RSSI at the module
// (dBm, signed), byte 2: the link quality % the receiver reports (0xFF:
// none in this reply), then 2 bytes per slot from the first, as the sensor
// sent them. Slot 0 = the receiver's own: byte A bits 7..6 link bars
// (0..3), bit 5 low voltage alarm, bit 2 the half (1: byte B is something
// else, e.g. FASSTest's EXT voltage); byte B the receiver's voltage in 0.1 V.
// FASSTest 18CH: 8 slots from 8 x page; 12CH: slot 0 and the link quality;
// T-FHSS Surface: 4 slots from 4 x group, the link quality in every other
// reply; Air: 8 slots from 8 x group
#define MPM_TELEM_FUTABA 0x13
// Jeti Duplex receiver replies (proto_jeti.c): mpm-v2's own. Byte 0, the
// kind: MPM_JETI_REPLY: byte 1 the reply's RSSI at the module (dBm,
// signed), bytes 2-7 the reply's bytes 2-7 (A: frames received, LE16; B,
// LE16: bits 15-14 01 = the receiver's voltage in 10 mV, 00 = a counter;
// flags: bits 1-0 antenna, bit 2 bind plug, bit 7 link new; the active
// antenna's signal level); MPM_JETI_SCREEN: bytes 1-32 the receiver's
// JETIBOX screen (2 lines of 16, 0 = space), while the radio's frames
// carry the JETIBOX byte: buttons L D U R in bits 7-4 (1 = pressed), a
// counter in bits 3-0 (a new value: one press); MPM_JETI_EX: from byte 1
// one message of a sensor on the receiver's Ext as it sent it (bit 8
// dropped), 0x7E first: an EX packet (byte 2 0xNF, its CRC8 checked; JETI
// Telemetry protocol 1.06) or an alarm (0x7E 0xN2 0x22|0x23 letter)
#define MPM_TELEM_JETI 0x14
#define MPM_JETI_REPLY 0
#define MPM_JETI_SCREEN 1
#define MPM_JETI_EX 2
// MPM v2 only, on the USB link: file_server.h replies (not an MPM type)
#define MPM_TELEM_FILE 0x40
// MPM v2 only, on the USB link: debug log text (log.h)
#define MPM_TELEM_LOG 0x41

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
