#pragma once

#include <stdint.h>

// Init GPT2-A, GPT2-B and GPT3
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

// Alarm at an absolute get_ticks() time, up to 2^31 ticks (44 s) ahead: the
// callback runs once, in the GPT3 interrupt, and may set the next alarm. A
// time already past: at once. Absolute times let a periodic task add its
// period to the last deadline, without drift
void timer_set_alarm(uint32_t at_ticks, void (*callback)());

// Cancel the alarm
void timer_cancel_alarm();
