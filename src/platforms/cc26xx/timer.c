#include <driverlib/timer.h>
#include <driverlib/prcm.h>
#include <driverlib/interrupt.h>

#include "timer.h"

#define MS_TIMER_BASE    GPT2_BASE
#define MS_TIMER_PERIPH  PRCM_PERIPH_TIMER2

#define TICKS_TIMER_BASE   GPT3_BASE
#define TICKS_TIMER_PERIPH PRCM_PERIPH_TIMER3

// Closer than this to the alarm time when setting it: the match may be
// missed, take the interrupt at once
#define ALARM_MIN_TICKS 48

static volatile uint32_t _ms_ticks = 0;
static void (*volatile _alarm_callback)();
static volatile uint32_t _alarm_at;

static void ms_int_handler() {
  TimerIntClear(MS_TIMER_BASE, TIMER_TIMA_TIMEOUT);
  ++_ms_ticks;
}

static void alarm_int_handler();

static inline uint16_t _read_micros() { return HWREGH(MS_TIMER_BASE + GPT_O_TAR); }

// Uses:
// - GPT2-A: millis() & micros()
// - GPT3: free running @ 48MHz, its match interrupt for the alarm
//
void timer_init()
{
  PRCMPeripheralRunEnable(MS_TIMER_PERIPH);
  PRCMPeripheralRunEnable(TICKS_TIMER_PERIPH);
  PRCMLoadSet();

  // GPT2-A prescaler @ 1Mhz
  TimerPrescaleSet(MS_TIMER_BASE, TIMER_A, 48 - 1);

  // GPT2-A: periodic
  TimerConfigure(MS_TIMER_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC);
  TimerStallControl(MS_TIMER_BASE, TIMER_BOTH, true);

  // GPT2-A: preload 1000 (1 ms)
  TimerLoadSet(MS_TIMER_BASE, TIMER_A, 1000 - 1);

  // GPT2-A: setup IRQ & enable
  TimerIntRegister(MS_TIMER_BASE, TIMER_A, ms_int_handler);
  TimerIntEnable(MS_TIMER_BASE, TIMER_TIMA_TIMEOUT);
  TimerEnable(MS_TIMER_BASE, TIMER_A);

  // GPT3: 32bit up-counter, match interrupt (alarm)
  TimerConfigure(TICKS_TIMER_BASE, TIMER_CFG_PERIODIC_UP);
  HWREG(TICKS_TIMER_BASE + GPT_O_TAMR) |= GPT_TAMR_TAMIE;
  TimerStallControl(TICKS_TIMER_BASE, TIMER_BOTH, true);
  TimerIntRegister(TICKS_TIMER_BASE, TIMER_A, alarm_int_handler);
  TimerIntEnable(TICKS_TIMER_BASE, TIMER_TIMA_MATCH);
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

// Match, or pended by timer_set_alarm(): early is a stale one (the match
// of an earlier alarm), the match comes later
static void alarm_int_handler()
{
  HWREG(TICKS_TIMER_BASE + GPT_O_ICLR) = GPT_ICLR_TAMCINT;
  void (*callback)() = _alarm_callback;
  if (!callback || ticks_before(_alarm_at)) return;
  _alarm_callback = 0;
  callback();
}

void timer_set_alarm(uint32_t at_ticks, void (*callback)())
{
  IntDisable(INT_GPT3A);
  _alarm_at = at_ticks;
  _alarm_callback = callback;
  HWREG(TICKS_TIMER_BASE + GPT_O_TAMATCHR) = at_ticks;
  if (_timer_diff(at_ticks, get_ticks()) < ALARM_MIN_TICKS) IntPendSet(INT_GPT3A);
  IntEnable(INT_GPT3A);
}

void timer_cancel_alarm() { _alarm_callback = 0; }
