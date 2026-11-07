/*
 * S.BUS2 monitor on a Raspberry Pi Pico: the receiver's S.BUS2 line on
 * SBUS2_PIN (inverted UART, 100 kbaud 8E2, read by a PIO state machine;
 * weak pull-down, the line idles low). Bytes are grouped by the gaps
 * between them: the S.BUS frame, then the 3-byte telemetry slots, each a
 * group of its own. Each group goes to the USB serial as
 *
 *   @g <t_us> <hex> [err=<n>]
 *
 * t_us: the start of its first byte (Pico timer), err: bytes with a parity
 * or stop bit error. Core 1 reads the line, core 0 prints.
 */
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "sbus_rx.pio.h"

#define BAUD 100000
#define GAP_US 200    // more than this between two bytes' starts beyond one byte: a new group
#define BYTE_US 120   // 8E2: 12 bits
#define PUSH_US 115   // from the start bit to the push (the stop bit's middle + 1 bit)
#define GROUP_MAX 40  // S.BUS frame: 25 bytes

typedef struct {
  uint32_t t_us;
  uint8_t n, err;
  uint8_t b[GROUP_MAX];
} group_t;

static queue_t _groups;
static PIO _pio = pio0;
static uint _sm;

static void _rx_init(void)
{
  uint offset = pio_add_program(_pio, &sbus_rx_program);
  _sm = pio_claim_unused_sm(_pio, true);
  pio_sm_config c = sbus_rx_program_get_default_config(offset);
  sm_config_set_in_pins(&c, SBUS2_PIN);
  sm_config_set_in_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
  pio_gpio_init(_pio, SBUS2_PIN);
  pio_sm_set_consecutive_pindirs(_pio, _sm, SBUS2_PIN, 1, false);
  gpio_pull_down(SBUS2_PIN);
  gpio_set_inover(SBUS2_PIN, GPIO_OVERRIDE_INVERT);
  pio_sm_init(_pio, _sm, offset, &c);
  pio_sm_set_enabled(_pio, _sm, true);
}

// Core 1: bytes from the PIO, grouped, to the queue (dropped if it's full)
static void _core1(void)
{
  group_t g = {0};
  uint32_t t_last = 0;
  for (;;) {
    if (g.n && time_us_32() - t_last > BYTE_US + GAP_US) {
      queue_try_add(&_groups, &g);
      g.n = g.err = 0;
    }
    if (pio_sm_is_rx_fifo_empty(_pio, _sm)) continue;
    uint32_t t = time_us_32() - PUSH_US;
    uint32_t w = pio_sm_get(_pio, _sm) >> 22;
    uint8_t d = w & 0xFF;
    bool bad = !(w >> 9 & 1) || ((__builtin_popcount(d) + (w >> 8 & 1)) & 1);
    if (g.n && t - t_last > BYTE_US + GAP_US) {
      queue_try_add(&_groups, &g);
      g.n = g.err = 0;
    }
    if (g.n == 0) g.t_us = t;
    if (g.n < GROUP_MAX) g.b[g.n++] = d;
    g.err += bad;
    t_last = t;
  }
}

int main(void)
{
  stdio_init_all();
  queue_init(&_groups, sizeof(group_t), 256);
  _rx_init();
  multicore_launch_core1(_core1);
  for (;;) {
    group_t g;
    queue_remove_blocking(&_groups, &g);
    char line[16 + 2 * GROUP_MAX + 16], *p = line;
    p += sprintf(p, "@g %lu ", (unsigned long)g.t_us);
    for (int i = 0; i < g.n; i++) p += sprintf(p, "%02x", g.b[i]);
    if (g.err) p += sprintf(p, " err=%u", g.err);
    puts(line);
  }
}
