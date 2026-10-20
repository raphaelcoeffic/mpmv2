#pragma once

// MULTI-Module core: takes the frames of the radio, runs the selected protocol
// and sends the module status back, after Multiprotocol.ino of the MPM
// (loop(), Update_All(), protocol_init(), update_serial_data())

#include <stdbool.h>
#include <stdint.h>

#include "mpm_frame.h"

// Version in the status telemetry: the MPM release whose serial link and
// telemetry this follows. EdgeTX warns below 1.3.3.0
#define MPM_VERSION_MAJOR 1
#define MPM_VERSION_MINOR 3
#define MPM_VERSION_REVISION 4
#define MPM_VERSION_PATCH 74

// No frame from the radio for this long: input signal lost, the protocol
// callbacks stop
#define MPM_INPUT_TIMEOUT_MS 70
// Status telemetry period
#define MPM_STATUS_PERIOD_MS 500
// The first callback of a protocol, after its init
#define MPM_FIRST_CALLBACK_US 5000
// Input frames are processed between callbacks if this much time is left
#define MPM_UPDATE_MIN_US 900
// Or after this many callbacks without time for it
#define MPM_UPDATE_FORCE 10
// The RF chips' work between callbacks (mpm_protocol_idle()) if this much
// time is left: the longest of it, a received packet's decoding
#define MPM_IDLE_MIN_US 1500

// Channels 5 to 12 above this: Channel_AUX bits
#define MPM_CHANNEL_SWITCH 1106

#define MPM_PROTO_LIST 0

// PPM input: no dial to pick the protocol, so one entry of the MPM's
// PPM_prot[] table, set at build time (FrSky X2 by default). Channels in
// the PPM order, no failsafe, no bind, no telemetry
#ifndef MPM_PPM_PROTOCOL
#define MPM_PPM_PROTOCOL 64
#endif
#ifndef MPM_PPM_SUB_PROTOCOL
#define MPM_PPM_SUB_PROTOCOL 0
#endif
#ifndef MPM_PPM_RX_NUM
#define MPM_PPM_RX_NUM 0
#endif
#ifndef MPM_PPM_OPTION
#define MPM_PPM_OPTION 0
#endif
// PPM widths for -100% and +100% (us): EdgeTX's 1500 +- 512, as the MPM's
// TX_ER9X. Up to MPM_CHANNEL_MIN..MAX, clamped to 0..2047 (+-125%)
#define MPM_PPM_MIN_100 988
#define MPM_PPM_MAX_100 2012

// As the MPM's mm_protocol_definition (Multi_Protos.ino)
typedef struct {
  uint8_t id;             // protocol number of the serial link
  const char* name;       // up to 7 characters
  // Sub protocol names: the length of a name, then n_sub names of that
  // length (the MPM's STR_SUBTYPE_*). NULL if none
  const char* sub_names;
  uint8_t n_sub;          // sub protocols, 0 = none
  uint8_t option_type;    // mpm_option_type_t
  bool failsafe;          // supports failsafe
  bool ch_map;            // supports disabling the channel mapping
  // Starts the protocol (mpm state set: sub protocol, option, id, bind...).
  // May reject the sub protocol: mpm_sub_invalid()
  void (*init)();
  // Called again after the returned number of microseconds
  uint16_t (*callback)();
  // The radio clears the bind flag, NULL = mpm_bind_done()
  void (*end_bind)();
} mpm_protocol_t;

// The protocol table, in the order the radio lists them
extern const mpm_protocol_t mpm_protocols[];
extern const uint8_t mpm_protocol_count;

typedef struct {
  // From the radio
  uint8_t protocol;
  uint8_t sub_protocol;
  uint8_t rx_num;
  int8_t option;
  bool autobind;
  bool range_check;
  bool low_power;
  bool disable_telem;
  bool disable_ch_map;  // only if the protocol supports it
  bool telem_invert;
  uint16_t channels[MPM_CHANNELS];  // 0..2047, MPM_CHANNEL_MIN..MAX = -+100%
  uint16_t failsafe[MPM_CHANNELS];  // MPM_FAILSAFE_NO_PULSE, _HOLD or a value
  bool failsafe_valid;              // failsafe values received since init
  uint8_t channel_aux;              // bit i: channel 5 + i above MPM_CHANNEL_SWITCH
  uint8_t data[MPM_FRAME_DATA_MAX]; // protocol data of the last frame
  uint8_t data_len;
  // Module
  uint32_t id;               // module id + rx_num
  uint32_t id_master;        // module id (MProtocol_id_master)
  uint8_t rx_tx_addr[5];     // from id, as set_rx_tx_addr()
  bool bind_in_progress;
  bool input;                // frames from the radio
  bool proto_valid;          // protocol and sub protocol found and started
  // Option type shown by the radio instead of the protocol's (0..15),
  // 0xFF = the protocol's. Reset at each protocol start
  uint8_t option_override;
} mpm_state_t;

extern mpm_state_t mpm;

void mpm_init();

// One step of the main loop: call it continuously. Runs the protocol
// callback when due, processes the radio frames in between, then the board's
// work (mpm_port_idle()). Also called from the waits of that work
void mpm_poll();

// Main loop, never returns
void mpm_run();

// For protocols
void mpm_bind_done();
void mpm_sub_invalid();
bool mpm_sub_valid();
// Telemetry frame of the given type: header + data
void mpm_telem_send(uint8_t type, const uint8_t* data, uint8_t len);
