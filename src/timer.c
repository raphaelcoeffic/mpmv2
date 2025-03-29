#include <driverlib/timer.h>
#include <driverlib/prcm.h>

static volatile uint32_t _ms_ticks = 0;

static void timer0_int_handler() {
  TimerIntClear(GPT0_BASE, TIMER_TIMA_TIMEOUT);
  ++_ms_ticks;
}

static inline uint16_t _read_micros() { return HWREGH(GPT0_BASE + GPT_O_TAR); }

// Uses GPT0-A
void timer_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_TIMER0);
  PRCMLoadSet();

  TimerLoadSet(GPT0_BASE, TIMER_A, 1000 - 1);
  TimerPrescaleSet(GPT0_BASE, TIMER_A, 48 - 1);
  TimerConfigure(GPT0_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PERIODIC);
  TimerIntRegister(GPT0_BASE, TIMER_A, timer0_int_handler);
  TimerIntEnable(GPT0_BASE, TIMER_TIMA_TIMEOUT);
  TimerEnable(GPT0_BASE, TIMER_A);
}

uint32_t millis() { return _ms_ticks; }

uint32_t micros() {
  uint32_t ms, us;

  do {
    ms = _ms_ticks;
    us = _read_micros();
    __asm__("nop \n"
            "nop \n");
  } while (ms != _ms_ticks);

  return (ms + 1) * 1000 - us;
}

