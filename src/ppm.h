#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PULSE_MIN_US  800
#define PULSE_MAX_US 2200

#define PPM_MIN_CHANNELS 8
#define PPM_MAX_CHANNELS 16

// TODO:
// - allow user provided channel buffer?
// - what about callbacks?

// uses GPT2 A
void ppm_timer_init(uint32_t pin);

// start detecting PPM signal
void ppm_start_detect();
void ppm_stop_detect();

// return true if detection is done
bool ppm_detect_done();

// start / stop normal PPM timer
void ppm_timer_start();
void ppm_timer_stop();

// number of valid channels
uint32_t ppm_get_valid_channels();

// array of channel values
const uint16_t* ppm_get_channels();
