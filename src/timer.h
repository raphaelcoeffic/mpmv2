#pragma once

#include <stdint.h>

// Init GPT0-A, GPT0-B and GPT1
void timer_init();

// Get milliseconds
uint32_t millis();

// Get microseconds
uint32_t micros();

// Get ticks timer (48 MHz)
uint32_t get_ticks();

// Microsecond delay
void delay_us(uint32_t us);

// Convert microseconds into ticks
#define us2ticks(us) (us * 48)

// Comparison macros
#define _timer_diff(t1, t2) ((int32_t)(t1 - t2))
#define _timer_before(t1, t2) (_timer_diff(t1, t2) < 0)
#define _timer_after(t1, t2) (_timer_diff(t1, t2) >= 0)

#define millis_before(ms) _timer_before(millis(), ms)
#define millis_after(ms) _timer_after(millis(), ms)

#define micros_before(ms) _timer_before(micros(), ms)
#define micros_after(ms) _timer_after(micros(), ms)

#define ticks_before(ms) _timer_before(get_ticks(), ms)
#define ticks_after(ms) _timer_after(get_ticks(), ms)

// Start timeout
void timer_start_timeout(uint32_t timeout_us, void (*callback)());

// Stop timeout
void timer_stop_timeout();
