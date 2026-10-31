// The radio bench (bench_port.h) on the CC2755P10 LaunchPad. RF activity
// LED: TX red, RX green, bad CRC both. No timing edges: no PA / LNA enable
// capture on this board yet
#include <ti/drivers/dpl/HwiP.h>

#include "bench_port.h"
#include "board.h"

const char bench_port_dev[] = "cc2755";

bool bench_port_irq_disable() { return HwiP_disable() != 0; }

void bench_port_irq_enable() { HwiP_restore(0); }

void bench_port_led(bench_led_t state)
{
  board_led(BOARD_LED_RED, state == BENCH_LED_TX || state == BENCH_LED_RX_BAD);
  board_led(BOARD_LED_GREEN, state == BENCH_LED_RX || state == BENCH_LED_RX_BAD);
}

bool bench_port_timing(bool on)
{
  (void)on;
  return false;
}

bool bench_port_timing_lna() { return false; }
