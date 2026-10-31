#pragma once

// What the shared code sizes by the chip's RAM (radio.h, log.h; frame.h
// from RADIO_MAX_PKT_LEN): the CC2652's 80 KB, the CC2650's 20 KB
// (MCU_CC26X0)
#if defined(MCU_CC26X0)
// The basic RF core commands only: no CYRF6936 emulation
#define RADIO_MAX_PKT_LEN 255
#define LOG_SIZE 1024  // power of 2
#else
// The advanced RF core commands above 255 bytes (CYRF6936 SDR: 1 KB for 16)
#define RADIO_MAX_PKT_LEN 1100
#define LOG_SIZE 4096  // power of 2
#endif
