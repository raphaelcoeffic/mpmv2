// timer.h on the CC27xx (platforms/cc26xx/timer.c on the CC2652), on the
// system timer (SYSTIM): micros() its 1 us time, get_ticks() its 250 ns time
// times 12 (48 MHz ticks, 0.25 us resolution), extended to 64 bits so that
// the ticks wrap at 2^32 (the 250 ns time wraps every 1073 s: get_ticks() or millis()
// must run once in that time). The alarm: SYSTIM channel 0 in compare mode,
// its event on CPU IRQ 16, as TI's ClockP (kernel/nortos/dpl/
// ClockPLPF3_nortos.c); rf.c has channel 2, the sync capture channel 4

#include "timer.h"

#include <stdbool.h>

#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_evtsvt.h>
#include <inc/hw_systim.h>
#include <inc/hw_ints.h>
#include <driverlib/interrupt.h>
#include <ti/drivers/dpl/HwiP.h>

#define TICKS_PER_250NS 12

static uint32_t _last, _high;
static void (*volatile _alarm)();

static uint64_t _time250n()
{
  uintptr_t key = HwiP_disable();
  uint32_t now = HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N);
  if (now < _last) _high++;
  _last = now;
  uint64_t t = (uint64_t)_high << 32 | now;
  HwiP_restore(key);
  return t;
}

// The channel's event (the CPU interrupt's level) stays set after the
// compare: as ClockP, disarm, read the compare value (out of compare mode
// that clears the event), clear the pending interrupt. ICLR only clears RIS
static void _disarm()
{
  HWREG(SYSTIM_BASE + SYSTIM_O_ARMCLR) = SYSTIM_ARMCLR_CH0_CLR;
  (void)HWREG(SYSTIM_BASE + SYSTIM_O_CH0CC);
  HWREG(SYSTIM_BASE + SYSTIM_O_ICLR) = SYSTIM_ICLR_EV0;
  IntClearPend(INT_CPUIRQ16);
}

static void _alarm_isr()
{
  _disarm();
  void (*cb)() = _alarm;
  _alarm = 0;
  if (cb) cb();
}

void timer_init()
{
  _time250n();
  // Idle (compare mode once a value is written), 250 ns resolution; SYSTIM
  // stops while the CPU is halted (debugger)
  HWREG(SYSTIM_BASE + SYSTIM_O_CH0CFG) = SYSTIM_CH0CFG_RES_NS;
  HWREG(SYSTIM_BASE + SYSTIM_O_EMU) = SYSTIM_EMU_HALT_STOP;
  HWREG(EVTSVT_BASE + EVTSVT_O_CPUIRQ16SEL) = EVTSVT_CPUIRQ16SEL_PUBID_SYSTIM0;
  timer_cancel_alarm();
  HWREG(SYSTIM_BASE + SYSTIM_O_IMSET) = SYSTIM_IMSET_EV0;
  IntRegister(INT_CPUIRQ16, _alarm_isr);
  IntClearPend(INT_CPUIRQ16);
  IntEnable(INT_CPUIRQ16);
}

uint32_t millis() { return (uint32_t)(_time250n() / 4000); }

uint32_t micros() { return HWREG(SYSTIM_BASE + SYSTIM_O_TIME1U); }

uint32_t get_ticks() { return (uint32_t)(_time250n() * TICKS_PER_250NS); }

void delay_us(uint32_t us)
{
  uint32_t start = micros();
  while (micros() - start < us) {
  }
}

void timer_set_alarm(uint32_t at_ticks, void (*callback)())
{
  timer_cancel_alarm();
  int32_t ahead = _timer_diff(at_ticks, get_ticks());
  if (ahead < TICKS_PER_250NS) ahead = TICKS_PER_250NS;
  _alarm = callback;
  uint32_t at = HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N) + (uint32_t)ahead / TICKS_PER_250NS;
  // Writing the compare value arms the channel, in compare mode
  HWREG(SYSTIM_BASE + SYSTIM_O_CH0CC) = at;
  // Already past (a short delay): the interrupt by hand
  if ((int32_t)(HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N) - at) >= 0 &&
      !(HWREG(SYSTIM_BASE + SYSTIM_O_RIS) & SYSTIM_RIS_EV0))
    IntSetPend(INT_CPUIRQ16);
}

void timer_cancel_alarm()
{
  _alarm = 0;
  _disarm();
}
