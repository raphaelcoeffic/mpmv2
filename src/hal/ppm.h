#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PULSE_MIN_US  800
#define PULSE_MAX_US 2200

#define PPM_MIN_CHANNELS 4
#define PPM_MAX_CHANNELS 16

typedef void (*ppm_callback_t)(const uint16_t* ppm, uint32_t channels);

// uses GPT0 A (edge time capture on the pin) and B (frame timeout), and
// uDMA channel 9 for the detection
void ppm_timer_init(uint32_t pin);

// start / stop normal PPM timer
void ppm_timer_start();
void ppm_timer_stop();

// call back 'us' after the last channel pulse, from the interrupt. Starts
// on a sync gap (> PULSE_MAX_US): a pulse below PULSE_MIN_US cancels the
// frame until the next sync (channels then 0)
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

// return true once pulse sampling is done (16 intervals, after a gap or
// 32 short ones). Never if the edges stop: ppm_timer_stop() and start again
bool ppm_detect_done();

//
// PPM detect stats:
//   detect_pulse_min: minimum pulse width
//   detect_pulse_cnt: number of edges before starting DMA
// 
extern uint16_t detect_pulse_min;
extern uint16_t detect_pulse_cnt;
