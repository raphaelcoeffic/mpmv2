#pragma once

// The protocols and options of the port, as _Config.h / Validate.h of the MPM

#define TELEMETRY
#define MULTI_TELEMETRY
#define HUB_TELEMETRY
#define SPORT_TELEMETRY
#define SPORT_SEND  // Validate.h: with SPORT_TELEMETRY
#define FAILSAFE_ENABLE
// The DSM bind details in the log (DSM_cyrf6936: bind started, the
// receiver's response or its timeout, clone ID): a few lines a bind
#define DEBUG_BIND 1
// FrSky's frequency plans (FrSkyDVX_common.ino): the CC26xx radios sit at
// the nominal frequency. X and X2 on FrSky's CC26xx plan with
// FRSKY_CC26XX_PLAN (CMake option, on by default); D and V, and X / X2
// without it, 25 FSCTRL0 steps (40 kHz) lower, where stock FrSky CC2500 gear
// listens (X8R, S8R, V8R4-II: docs/frsky-frequency.md of mpm-v2)
#define FRSKY_FSCTRL0_OFFSET -25

// FrSkyRX's tune search, FSCTRL0 steps either way (127 upstream). The
// CC2755 receives FrSky D (31 kbps: samples at 248 kHz) 248 kHz off the
// channel as well (156 steps, docs/bench.md of mpm-v2), each window about
// +-28 steps at bind power: +-80 keeps the aliases out and the channel in
// for a transmitter within 48 steps (76 kHz) of the CC2755
#ifdef BOARD_CC2755P10_LP
#define FRSKY_RX_TUNE_RANGE 80
#endif

#if defined(MPM_REDUCED)
// The reduced MPM (the XJT-Lite's board.cmake; CC2650, 20 KB of RAM): FrSky
// D and X, and the WLtoys cars' V2x2 and XK
#define NRF24L01_INSTALLED
#define NRF24L01_ENABLE_LOW_POWER
#define V2X2_NRF24L01_INO
#define XK_CCNRF_INO

#define CC2500_INSTALLED
#define CC2500_ENABLE_LOW_POWER
#define FRSKYD_CC2500_INO
#define FRSKYX_CC2500_INO
#else
// NRF24L01 / XN297 family: NRF24L01 and XN297 emulations on the CC2652
// (nrf24l01.c, xn297.c)
#define NRF24L01_INSTALLED
#define NRF24L01_ENABLE_LOW_POWER

#define BAYANG_NRF24L01_INO
#define V2X2_NRF24L01_INO
#define XK_CCNRF_INO
#define MJXQ_CCNRF_INO
#ifdef MPM_SKETCH
// Untested: no model on the bench. Their ESB acks (nrf24l01.c's EN_AA)
// checked against an NRF24L01 as PRX only: CFlie's search and commander
// packets at 250k, 1M and 2M, Propel's bind (the ID in the ack payload).
// The MULTI-Module repo's build only (mpm/sketch): no C copies here
#define CFLIE_NRF24L01_INO
#define PROPEL_NRF24L01_INO
#endif

// Futaba FASST 10ch, on the radio directly (proto_fasst.c, core/fasst_frame.c)
#define FASST_INO

#define BAYANG_HUB_TELEMETRY
#define BAYANG_RX_NRF24L01_INO
#define BAYANG_RX_TELEMETRY  // the channels received to the radio

// CC2500 family: CC2500 emulation on the CC2652 (cc2500.c)
#define CC2500_INSTALLED
#define CC2500_ENABLE_LOW_POWER

#define FRSKYD_CC2500_INO
#define FRSKYV_CC2500_INO
#define FRSKYX_CC2500_INO
#define FUTABA_CC2500_INO
// FrSky RX, its clone subtype: the TX ID and hop table of FrSky D / X / X2
// for their "Cloned" subtypes
#define FRSKY_RX_CC2500_INO
#define FRSKY_RX_TELEMETRY

// A7105 family: A7105 emulation on the CC2652 (a7105.c)
#define A7105_INSTALLED
#define A7105_ENABLE_LOW_POWER
#define FORCE_AFHDS2A_TUNING 0  // no channel 15 tuning (Validate.h)

#define FLYSKY_A7105_INO
#define AFHDS2A_A7105_INO
#define AFHDS2A_FW_TELEMETRY
#define AFHDS2A_RX_A7105_INO
#define AFHDS2A_RX_TELEMETRY

// CYRF6936 family: CYRF6936 emulation on the CC2652 (cyrf6936.c)
#define CYRF6936_INSTALLED
#define CYRF6936_ENABLE_LOW_POWER

#define DSM_CYRF6936_INO
#define DSM_TELEMETRY
#define DSM_X_PLUS               // _Config.h defaults
#define DSM_THROTTLE_KILL_CH 14
// DSM RX, its clone subtype: the TX ID for DSM's clone option
#define DSM_RX_CYRF6936_INO
#endif
