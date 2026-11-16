#pragma once

#include <stdbool.h>
#include <stdint.h>

// Jeti Duplex 2.4 (old PPM module, AT86RF230) follower: 802.15.4 O-QPSK
// received as MSK at 2 Mchip/s, despread in software (doc/jeti.md). Every
// frame printed as
//   @jf <t_sfd> <ch> rssi=<dBm> d=<worst symbol distance> fcs=<0|1> <psdu>
// t_sfd: micros() at the start of the SFD; ch: IEEE channel (11..26);
// psdu: as many bytes as the capture holds. A module data frame while
// locked adds
//   off=<us>: its time against the hop clock's phase (bits corrected)
// The module hops on a clock of its own, one step every 10 ms around the
// ring of its 5-channel hop list (payload bytes 5..7, the current channel
// last, the next step's channel first), and sends one frame per PPM frame
// on the current step's channel, -3.5..+4.7 ms around the step's mean
// frame time (byte 5 bits 5..6: 3 early, 2 middle, 1 late). With follow, a
// module data frame (37 B and more, type low nibble 1) locks: the ring
// from its list, the phase from its time. The RX hops with the clock, as
// the receiver does; after a module frame it stays on the channel for the
// receiver's reply (4 ms). 0.3 s without a module frame: parked on ch again
typedef struct {
  uint32_t secs;        // 0: until stopped
  uint8_t ch;           // IEEE channel to start on (11..26)
  uint16_t frame_len;   // air bytes per module frame capture (after the sync)
  bool follow;
  // Burst test (carrier sense): after every burst_every-th module frame, a
  // burst_len byte burst of noise on its channel burst_at us after its SFD
  // (then the reply listened to as usual), printed as "@jb <t_sfd> <status>"
  uint16_t burst_at;    // 0: none
  uint16_t burst_len;
  uint8_t burst_every;
  bool (*stop)();       // true: end the run now (the host asked), or NULL
} jeti_rx_cfg_t;

typedef void (*jeti_print_t)(const char* fmt, ...);

// Blocks for cfg->secs or until cfg->stop(); the radio needs radio_init()
// again after it
void jeti_rx_run(const jeti_rx_cfg_t* cfg, jeti_print_t print);
