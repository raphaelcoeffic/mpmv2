#pragma once

// The protocols and options of the port, as _Config.h / Validate.h of the MPM

#define TELEMETRY
#define MULTI_TELEMETRY
#define HUB_TELEMETRY
#define SPORT_TELEMETRY
#define FAILSAFE_ENABLE

// NRF24L01 / XN297 family: XN297 emulation on the CC2652 (xn297.c)
#define NRF24L01_INSTALLED

#define BAYANG_NRF24L01_INO
#define BAYANG_HUB_TELEMETRY

// CC2500 family: CC2500 emulation on the CC2652 (cc2500.c)
#define CC2500_INSTALLED
#define CC2500_ENABLE_LOW_POWER

#define FRSKYD_CC2500_INO
#define FRSKYV_CC2500_INO
#define FRSKYX_CC2500_INO

// A7105 family: A7105 emulation on the CC2652 (a7105.c)
#define A7105_INSTALLED
#define A7105_ENABLE_LOW_POWER
#define FORCE_AFHDS2A_TUNING 0  // no channel 15 tuning (Validate.h)

#define AFHDS2A_A7105_INO
#define AFHDS2A_FW_TELEMETRY
