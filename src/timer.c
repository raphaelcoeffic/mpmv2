#include <driverlib/timer.h>
#include <driverlib/prcm.h>

#include "timer.h"

static volatile uint32_t _ms_ticks = 0;
static void (*_timeout_callback)() = 0;

static void timer0_int_handler() {
  TimerIntClear(GPT0_BASE, TIMER_TIMA_TIMEOUT);
  ++_ms_ticks;
}

static void timeout_int_handler();

static inline uint16_t _read_micros() { return HWREGH(GPT0_BASE + GPT_O_TAR); }

// Uses:
// - GPT0-A: millis() & micros()
// - GPT0-B: one-shots in microseconds (up to ~65ms)
// - GPT1: free running @ 48MHz
// 
void timer_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_TIMER0);
  PRCMPeripheralRunEnable(PRCM_PERIPH_TIMER1);
  PRCMLoadSet();

  // GPT0-A & GPT0-B prescaler @ 1Mhz
  TimerPrescaleSet(GPT0_BASE, TIMER_BOTH, 48 - 1);

  // GPT0-A: periodic
  // GPT0-B: one-shot
  TimerConfigure(GPT0_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC |
                                TIMER_CFG_B_ONE_SHOT);
  TimerStallControl(GPT0_BASE, TIMER_BOTH, true);

  // GPT0-A: preload 1000 (1 ms)
  TimerLoadSet(GPT0_BASE, TIMER_A, 1000 - 1);

  // GPT0-A: setup IRQ & enable
  TimerIntRegister(GPT0_BASE, TIMER_A, timer0_int_handler);
  TimerIntEnable(GPT0_BASE, TIMER_TIMA_TIMEOUT);
  TimerEnable(GPT0_BASE, TIMER_A);

  // GPT0-B: prepare IRQ
  TimerIntRegister(GPT0_BASE, TIMER_B, timeout_int_handler);

  // GPT1: 32bit up-counter
  TimerConfigure(GPT1_BASE, TIMER_CFG_PERIODIC_UP);
  TimerStallControl(GPT1_BASE, TIMER_BOTH, true);
  TimerEnable(GPT1_BASE, TIMER_BOTH);
}

uint32_t millis() { return _ms_ticks; }

uint32_t micros() {
  uint32_t ms, us, ms2;

  do {
    ms = _ms_ticks;
    us = _read_micros();
    ms2 = _ms_ticks;
  } while (ms != ms2);

  return (ms + 1) * 1000 - us;
}

uint32_t get_ticks() {
  // GPT1: 32bit combined access
  return HWREG(GPT1_BASE + GPT_O_TAR);
}

void delay_us(uint32_t us)
{
  uint32_t t = get_ticks() + us2ticks(us);
  while (ticks_before(t)) {}
}

static void timeout_int_handler() {
  TimerIntClear(GPT0_BASE, TIMER_TIMB_TIMEOUT);
  TimerDisable(GPT0_BASE, TIMER_B);

  if (_timeout_callback) _timeout_callback();
}

void timer_start_timeout(uint32_t timeout_us, void (*callback)())
{
  TimerLoadSet(GPT0_BASE, TIMER_B, timeout_us - 1);
  TimerEnable(GPT0_BASE, TIMER_B);
}

void timer_stop_timeout()
{
  TimerDisable(GPT0_BASE, TIMER_B);
}
