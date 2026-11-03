// The radio bench (bench_port.h) on the ZBDongle-P: no LED, no PA / LNA
// enable pin for "timing"
#include <driverlib/interrupt.h>

#include "bench_port.h"

const char bench_port_dev[] = "cc2652p";

bool bench_port_irq_disable() { return IntMasterDisable(); }

void bench_port_irq_enable() { IntMasterEnable(); }

void bench_port_led(bench_led_t state) { (void)state; }

bool bench_port_timing(bool on)
{
  (void)on;
  return false;
}

bool bench_port_timing_lna() { return false; }
