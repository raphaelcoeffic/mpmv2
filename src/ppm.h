#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PULSE_MIN_US  800
#define PULSE_MAX_US 2200

#define PPM_MIN_CHANNELS 4
#define PPM_MAX_CHANNELS 16

typedef void (*ppm_callback_t)(const uint16_t* ppm, uint32_t channels);

// uses GPT2 A/B
void ppm_timer_init(uint32_t pin);

// start / stop normal PPM timer
void ppm_timer_start();
void ppm_timer_stop();

// call back 'us' after the last frame pulse
void ppm_timer_set_timeout(uint16_t us, ppm_callback_t cb);

// number of valid channels
uint32_t ppm_get_valid_channels();

// array of channel values
const uint16_t* ppm_get_channels();

//
// pulse detection methods
// 
void ppm_start_detect();
void ppm_stop_detect();

// return true once pulse sampling is done
bool ppm_detect_done();

//
// PPM detect stats:
//   detect_pulse_min: minimum pulse width
//   detect_pulse_cnt: number of edges before starting DMA
// 
extern uint16_t detect_pulse_min;
extern uint16_t detect_pulse_cnt;
