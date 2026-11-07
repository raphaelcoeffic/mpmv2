/*
 * S.BUS2 monitor and sensor emulator on a Raspberry Pi Pico: the
 * receiver's S.BUS2 line on SBUS2_PIN (inverted UART, 100 kbaud 8E2;
 * weak pull-down, the line idles low).
 *
 * RX: a PIO state machine reads the line. Bytes are grouped by the gaps
 * between them: the S.BUS frame, then the 3-byte telemetry slots, each a
 * group of its own. Each group goes to the USB serial as
 *
 *   @g <t_us> <hex> [err=<n>]
 *
 * t_us: the start of its first byte (Pico timer), err: bytes with a parity
 * or stop bit error. Our own slots come back the same way.
 *
 * TX: sensor slots. After a frame whose end byte is 0x04 / 0x14 / 0x24 /
 * 0x34 come slots 0..7 / 8..15 / 16..23 / 24..31, slot i of the group at
 * SLOT0_US + SLOT_US * i from the frame's start; a set slot goes out there
 * (its ID byte, then its 2 data bytes) from a second state machine, which
 * drives the pin only during its bytes. Slot 0 is the receiver's own.
 * Commands (USB serial, one per line):
 *
 *   slot <n> <hhhh>   slot n (0..31) sends data bytes hh hh
 *   slot <n> off      stops it
 *   clear             all off
 *   slots             lists the set slots (@slot <n> <hhhh>)
 *
 * Core 1 runs the line, core 0 the USB serial.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "sbus_rx.pio.h"
#include "sbus_tx.pio.h"

#define BAUD 100000
#define GAP_US 200    // more than this between two bytes' starts beyond one byte: a new group
#define BYTE_US 120   // 8E2: 12 bits
#define PUSH_US 115   // from the start bit to the push (the stop bit's middle + 1 bit)
#define GROUP_MAX 40  // S.BUS frame: 25 bytes
#define FRAME_LEN 25
#define SLOTS 32
#define SLOTS_PER_FRAME 8
#define SLOT0_US 5030  // slot 0 from the frame's start: the receiver's own (3 ms frame + 2 ms)
#define SLOT_US 660

typedef struct {
  uint32_t t_us;
  uint8_t n, err;
  uint8_t b[GROUP_MAX];
} group_t;

static queue_t _groups;
static PIO _pio = pio0;
static uint _sm_rx, _sm_tx;
// Slot data: bit 16 = set, bits 0..15 = the data bytes (first in bits 8..15)
static volatile uint32_t _slot[SLOTS];

static void _line_init(void)
{
  // RX
  uint offset = pio_add_program(_pio, &sbus_rx_program);
  _sm_rx = pio_claim_unused_sm(_pio, true);
  pio_sm_config c = sbus_rx_program_get_default_config(offset);
  sm_config_set_in_pins(&c, SBUS2_PIN);
  sm_config_set_in_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
  pio_gpio_init(_pio, SBUS2_PIN);
  gpio_pull_down(SBUS2_PIN);
  gpio_set_inover(SBUS2_PIN, GPIO_OVERRIDE_INVERT);
  gpio_set_outover(SBUS2_PIN, GPIO_OVERRIDE_INVERT);
  pio_sm_init(_pio, _sm_rx, offset, &c);

  // TX: released, idle level (stop bit) on its output
  offset = pio_add_program(_pio, &sbus_tx_program);
  _sm_tx = pio_claim_unused_sm(_pio, true);
  c = sbus_tx_program_get_default_config(offset);
  sm_config_set_out_pins(&c, SBUS2_PIN, 1);
  sm_config_set_set_pins(&c, SBUS2_PIN, 1);
  sm_config_set_sideset_pins(&c, SBUS2_PIN);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
  pio_sm_set_pins_with_mask(_pio, _sm_tx, 1u << SBUS2_PIN, 1u << SBUS2_PIN);
  pio_sm_set_pindirs_with_mask(_pio, _sm_tx, 0, 1u << SBUS2_PIN);
  pio_sm_init(_pio, _sm_tx, offset, &c);

  pio_sm_set_enabled(_pio, _sm_rx, true);
  pio_sm_set_enabled(_pio, _sm_tx, true);
}

// Slot i's ID byte: 0x03, the slot number bit-reversed in bits 3..7
static uint8_t _slot_id(int i)
{
  uint8_t r = 0;
  for (int b = 0; b < 5; b++)
    if (i >> b & 1) r |= 0x80 >> b;
  return r | 0x03;
}

static void _tx_byte(uint8_t d)
{
  pio_sm_put(_pio, _sm_tx, d | (uint32_t)(__builtin_popcount(d) & 1) << 8);
}

// Core 1: bytes from the PIO, grouped, to the queue (dropped if it's full);
// after each S.BUS2 frame, the set slots of its group, each at its time
static void _core1(void)
{
  group_t g = {0};
  uint32_t t_last = 0, t_frame = 0;
  int group = -1, next = SLOTS_PER_FRAME;  // the group's next slot to look at
  for (;;) {
    uint32_t now = time_us_32();
    if (g.n && now - t_last > BYTE_US + GAP_US) {
      queue_try_add(&_groups, &g);
      g.n = g.err = 0;
    }
    // The next set slot of the group, when its time comes (a late one is
    // dropped: it would run into the next slot)
    while (next < SLOTS_PER_FRAME) {
      uint32_t s = _slot[group * SLOTS_PER_FRAME + next];
      int32_t dt = (int32_t)(now - (t_frame + SLOT0_US + SLOT_US * next));
      if (!(s >> 16 & 1) || dt > 100) {
        next++;
        continue;
      }
      if (dt >= 0) {
        _tx_byte(_slot_id(group * SLOTS_PER_FRAME + next));
        _tx_byte(s >> 8);
        _tx_byte(s);
        next++;
      }
      break;
    }
    if (pio_sm_is_rx_fifo_empty(_pio, _sm_rx)) continue;
    uint32_t t = time_us_32() - PUSH_US;
    uint32_t w = pio_sm_get(_pio, _sm_rx) >> 22;
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
    // A whole S.BUS2 frame: its group of slots follows
    if (g.n == FRAME_LEN && g.b[0] == 0x0F && (d & 0x0F) == 0x04 && !g.err) {
      t_frame = g.t_us;
      group = d >> 4 & 3;
      next = 0;
    }
  }
}

static void _command(char* line)
{
  char* arg = strtok(line, " ");
  if (!arg) return;
  if (!strcmp(arg, "slot")) {
    char* n = strtok(NULL, " ");
    char* v = strtok(NULL, " ");
    int i = n ? atoi(n) : -1;
    if (i < 0 || i >= SLOTS || !v) {
      puts("@err usage: slot <n> <hhhh>|off");
    } else if (!strcmp(v, "off")) {
      _slot[i] = 0;
      printf("@ok slot %d off\n", i);
    } else {
      _slot[i] = 1u << 16 | (strtoul(v, NULL, 16) & 0xFFFF);
      printf("@ok slot %d %04lx\n", i, (unsigned long)(_slot[i] & 0xFFFF));
    }
  } else if (!strcmp(arg, "clear")) {
    for (int i = 0; i < SLOTS; i++) _slot[i] = 0;
    puts("@ok clear");
  } else if (!strcmp(arg, "slots")) {
    for (int i = 0; i < SLOTS; i++)
      if (_slot[i] >> 16 & 1) printf("@slot %d %04lx\n", i, (unsigned long)(_slot[i] & 0xFFFF));
    puts("@ok slots");
  } else {
    puts("@err commands: slot <n> <hhhh>|off, clear, slots");
  }
}

int main(void)
{
  stdio_init_all();
  queue_init(&_groups, sizeof(group_t), 256);
  _line_init();
  multicore_launch_core1(_core1);
  char cmd[64];
  int len = 0;
  for (;;) {
    group_t g;
    if (queue_try_remove(&_groups, &g)) {
      char line[16 + 2 * GROUP_MAX + 16], *p = line;
      p += sprintf(p, "@g %lu ", (unsigned long)g.t_us);
      for (int i = 0; i < g.n; i++) p += sprintf(p, "%02x", g.b[i]);
      if (g.err) p += sprintf(p, " err=%u", g.err);
      puts(line);
    }
    int c = getchar_timeout_us(0);
    if (c == '\r' || c == '\n') {
      cmd[len] = 0;
      if (len) _command(cmd);
      len = 0;
    } else if (c >= 0 && len < (int)sizeof(cmd) - 1) {
      cmd[len++] = c;
    }
  }
}
