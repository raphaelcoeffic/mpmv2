#pragma once

// What the shared code sizes by the chip's RAM (radio.h, log.h; frame.h
// from RADIO_MAX_PKT_LEN): the CC2755P10's 144 KB, the CC2652's sizes
// (rf.c streams the long packets through the FIFO)
#define RADIO_MAX_PKT_LEN 1100
#define LOG_SIZE 4096  // power of 2
