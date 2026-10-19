#include <driverlib/timer.h>
#include <driverlib/prcm.h>

#include "timer.h"

#define MS_TIMER_BASE    GPT2_BASE
#define MS_TIMER_PERIPH  PRCM_PERIPH_TIMER2

#define TICKS_TIMER_BASE   GPT3_BASE
#define TICKS_TIMER_PERIPH PRCM_PERIPH_TIMER3

static volatile uint32_t _ms_ticks = 0;
static void (*_timeout_callback)() = 0;

static void ms_int_handler() {
  TimerIntClear(MS_TIMER_BASE, TIMER_TIMA_TIMEOUT);
  ++_ms_ticks;
}

static void timeout_int_handler();

static inline uint16_t _read_micros() { return HWREGH(MS_TIMER_BASE + GPT_O_TAR); }

// Uses:
// - GPT2-A: millis() & micros()
// - GPT2-B: one-shots in microseconds (up to ~65ms)
// - GPT3: free running @ 48MHz
// 
void timer_init()
{
  PRCMPeripheralRunEnable(MS_TIMER_PERIPH);
  PRCMPeripheralRunEnable(TICKS_TIMER_PERIPH);
  PRCMLoadSet();

  // GPT0-A & GPT0-B prescaler @ 1Mhz
  TimerPrescaleSet(MS_TIMER_BASE, TIMER_BOTH, 48 - 1);

  // GPT2-A: periodic
  // GPT2-B: one-shot
  TimerConfigure(MS_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC |
                                TIMER_CFG_B_ONE_SHOT);
  TimerStallControl(MS_TIMER_BASE, TIMER_BOTH, true);

  // GPT2-A: preload 1000 (1 ms)
  TimerLoadSet(MS_TIMER_BASE, TIMER_A, 1000 - 1);

  // GPT2-A: setup IRQ & enable
  TimerIntRegister(MS_TIMER_BASE, TIMER_A, ms_int_handler);
  TimerIntEnable(MS_TIMER_BASE, TIMER_TIMA_TIMEOUT);
  TimerEnable(MS_TIMER_BASE, TIMER_A);

  // GPT2-B: prepare IRQ
  TimerIntRegister(MS_TIMER_BASE, TIMER_B, timeout_int_handler);

  // GPT3: 32bit up-counter
  TimerConfigure(TICKS_TIMER_BASE, TIMER_CFG_PERIODIC_UP);
  TimerStallControl(TICKS_TIMER_BASE, TIMER_BOTH, true);
  TimerEnable(TICKS_TIMER_BASE, TIMER_BOTH);
}

uint32_t millis() { return _ms_ticks; }

uint32_t micros() {
  uint32_t ms, us, ms2, pending;

  // The counter may have reloaded before the interrupt counted the
  // millisecond (time-out flag still set): count it here, or micros() goes
  // back by 1 ms and every "micros() - start > timeout" fires at once
  do {
    ms = _ms_ticks;
    us = _read_micros();
    pending = HWREG(MS_TIMER_BASE + GPT_O_RIS) & GPT_RIS_TATORIS;
    if (pending) us = _read_micros();
    ms2 = _ms_ticks;
  } while (ms != ms2);

  return (ms + 1 + (pending ? 1 : 0)) * 1000 - us;
}

uint32_t get_ticks() {
  // GPT1: 32bit combined access
  return HWREG(TICKS_TIMER_BASE + GPT_O_TAR);
}

void delay_us(uint32_t us)
{
  uint32_t t = get_ticks() + us2ticks(us);
  while (ticks_before(t)) {}
}

static void timeout_int_handler() {
  TimerIntClear(MS_TIMER_BASE, TIMER_TIMB_TIMEOUT);
  TimerDisable(MS_TIMER_BASE, TIMER_B);

  if (_timeout_callback) _timeout_callback();
}

void timer_start_timeout(uint32_t timeout_us, void (*callback)())
{
  TimerLoadSet(MS_TIMER_BASE, TIMER_B, timeout_us - 1);
  TimerEnable(MS_TIMER_BASE, TIMER_B);
}

void timer_stop_timeout()
{
  TimerDisable(MS_TIMER_BASE, TIMER_B);
}
