/*
 * JETIBOX emulator on a Raspberry Pi Pico: the device's JETIBOX line on
 * JBOX_PIN (a Jeti receiver's Ext socket: UART 9600 baud, 9 data bits, odd
 * parity, 2 stop bits, half duplex, idles high; pull-up).
 *
 * The device sends "simple text" packets: 0xFE (bit 8 = 0), 32 characters
 * (bit 8 = 1), 0xFF (bit 8 = 0), possibly with other messages around them
 * (EX telemetry, 0x7E ...), then releases the line; the JETIBOX answers
 * with one byte, 0bLDUR0000 (bit 8 = 0), a button's bit 0 while pressed
 * (doc/jeti_ref/telem.txt). We answer ANSWER_US after each packet's 0xFF.
 *
 * To the USB serial:
 *   @s <t_us> <n> |<line 1>|<line 2>|   a screen, when it changes (n: packets
 *                                      since the last one printed)
 *   @g <t_us> <words>                  other words, grouped by gaps, 3 hex
 *                                      digits each (bit 8 included)
 *   [err=<n>]                          words with a parity or stop bit error
 * Commands (one per line):
 *   k <l|d|u|r...> [n]   press these buttons for the next n answers (2)
 *   idle on|off          answer 0xF0 (nothing pressed) or stay silent
 *                        when no button is set (default on)
 *   screen               the last screen again
 *
 * Core 1 runs the line, core 0 the USB serial.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "jbox_rx.pio.h"
#include "jbox_tx.pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#define BAUD 9600
#define WORD_US 1250    // 12 bits
#define PUSH_US 1200    // from the start bit to the push (the stop bit's middle + 1 bit)
#define GAP_US 2000     // more than this between two words beyond one word: a new group
#define ANSWER_US 3000  // the button byte after the packet's end
#define ECHO_US 2000    // our own answer comes back: ignored for this long
#define GROUP_MAX 64
#define TEXT_LEN 32

typedef struct {
  uint32_t t_us;
  uint8_t n, err;
  uint16_t w[GROUP_MAX];
} group_t;

typedef struct {
  uint32_t t_us;
  char text[TEXT_LEN + 1];
} screen_t;

static queue_t _groups, _screens;
static PIO _pio = pio0;
static uint _sm_rx, _sm_tx;
static volatile uint8_t _press;     // buttons, bits LDUR in 7..4
static volatile uint8_t _press_n;   // answers left with them
static volatile bool _idle = true;

static void _line_init(void)
{
  uint offset = pio_add_program(_pio, &jbox_rx_program);
  _sm_rx = pio_claim_unused_sm(_pio, true);
  pio_sm_config c = jbox_rx_program_get_default_config(offset);
  sm_config_set_in_pins(&c, JBOX_PIN);
  sm_config_set_in_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
  pio_gpio_init(_pio, JBOX_PIN);
  gpio_pull_up(JBOX_PIN);
  pio_sm_init(_pio, _sm_rx, offset, &c);

  // TX: released, idle level (stop bit) on its output
  offset = pio_add_program(_pio, &jbox_tx_program);
  _sm_tx = pio_claim_unused_sm(_pio, true);
  c = jbox_tx_program_get_default_config(offset);
  sm_config_set_out_pins(&c, JBOX_PIN, 1);
  sm_config_set_set_pins(&c, JBOX_PIN, 1);
  sm_config_set_sideset_pins(&c, JBOX_PIN);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (8 * BAUD));
  pio_sm_set_pins_with_mask(_pio, _sm_tx, 1u << JBOX_PIN, 1u << JBOX_PIN);
  pio_sm_set_pindirs_with_mask(_pio, _sm_tx, 0, 1u << JBOX_PIN);
  pio_sm_init(_pio, _sm_tx, offset, &c);

  pio_sm_set_enabled(_pio, _sm_rx, true);
  pio_sm_set_enabled(_pio, _sm_tx, true);
}

// 9 data bits + odd parity
static void _tx_word(uint16_t w)
{
  uint32_t par = (__builtin_popcount(w & 0x1FF) & 1) ^ 1;
  pio_sm_put(_pio, _sm_tx, (w & 0x1FF) | par << 9);
}

// Core 1: words from the PIO; text packets to the screen queue, the rest
// grouped to the group queue (both dropped if full); the answer after each
// packet
static void _core1(void)
{
  group_t g = {0};
  screen_t s = {0};
  int text = -1;  // characters so far in a packet, -1 outside one
  uint32_t t_last = 0, t_answer = 0, t_echo = 0;
  bool answer = false;
  for (;;) {
    uint32_t now = time_us_32();
    if (g.n && now - t_last > WORD_US + GAP_US) {
      queue_try_add(&_groups, &g);
      g.n = g.err = 0;
    }
    if (answer && (int32_t)(now - t_answer) >= 0) {
      answer = false;
      uint8_t b = 0xF0;
      if (_press_n) {
        b &= ~_press;
        _press_n--;
      }
      if (b != 0xF0 || _idle) {
        _tx_word(b);
        t_echo = now + ECHO_US;
      }
    }
    if (pio_sm_is_rx_fifo_empty(_pio, _sm_rx)) continue;
    uint32_t t = time_us_32() - PUSH_US;
    uint32_t w = pio_sm_get(_pio, _sm_rx) >> 21;
    if ((int32_t)(t - t_echo) < 0) continue;
    uint16_t d = w & 0x1FF;
    bool bad = !(w >> 10 & 1) || !(__builtin_popcount(w & 0x3FF) & 1);
    if (!bad && d == 0x0FE) {
      text = 0;
      s.t_us = t;
      continue;
    }
    if (text >= 0 && !bad && (d >> 8)) {
      if (text < TEXT_LEN) s.text[text++] = d & 0xFF;
      continue;
    }
    if (text >= 0 && !bad && d == 0x0FF) {
      s.text[text] = 0;
      queue_try_add(&_screens, &s);
      text = -1;
      answer = true;
      t_answer = t + WORD_US + ANSWER_US;
      continue;
    }
    text = -1;
    if (g.n && t - t_last > WORD_US + GAP_US) {
      queue_try_add(&_groups, &g);
      g.n = g.err = 0;
    }
    if (g.n == 0) g.t_us = t;
    if (g.n < GROUP_MAX) g.w[g.n++] = d;
    g.err += bad;
    t_last = t;
  }
}

static void _command(char* line)
{
  char* arg = strtok(line, " ");
  if (!arg) return;
  if (!strcmp(arg, "k")) {
    char* keys = strtok(NULL, " ");
    char* n = strtok(NULL, " ");
    uint8_t m = 0;
    for (char* p = keys; p && *p; p++) {
      if (*p == 'l') m |= 0x80;
      else if (*p == 'd') m |= 0x40;
      else if (*p == 'u') m |= 0x20;
      else if (*p == 'r') m |= 0x10;
    }
    if (!m) {
      puts("@err usage: k <l|d|u|r...> [n]");
      return;
    }
    _press = m;
    _press_n = n ? atoi(n) : 2;
    printf("@ok k %02x %u\n", m, _press_n);
  } else if (!strcmp(arg, "idle")) {
    char* v = strtok(NULL, " ");
    _idle = !v || strcmp(v, "off");
    printf("@ok idle %s\n", _idle ? "on" : "off");
  } else if (!strcmp(arg, "screen")) {
    puts("@ok screen");
  } else {
    puts("@err commands: k <l|d|u|r...> [n], idle on|off, screen");
  }
}

static void _print_screen(const screen_t* s, unsigned n)
{
  char l[2][17];
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < 16; j++) {
      char c = s->text[16 * i + j];
      l[i][j] = c >= 0x20 && c < 0x7F ? c : (c ? '.' : ' ');
    }
    l[i][16] = 0;
  }
  printf("@s %lu %u |%s|%s|\n", (unsigned long)s->t_us, n, l[0], l[1]);
}

int main(void)
{
  stdio_init_all();
  queue_init(&_groups, sizeof(group_t), 64);
  queue_init(&_screens, sizeof(screen_t), 64);
  _line_init();
  multicore_launch_core1(_core1);
  char cmd[64];
  int len = 0;
  screen_t last = {0};
  unsigned since = 0;
  for (;;) {
    group_t g;
    if (queue_try_remove(&_groups, &g)) {
      char out[24 + 3 * GROUP_MAX + 16], *p = out;
      p += sprintf(p, "@g %lu ", (unsigned long)g.t_us);
      for (int i = 0; i < g.n; i++) p += sprintf(p, "%03x", g.w[i]);
      if (g.err) p += sprintf(p, " err=%u", g.err);
      puts(out);
    }
    screen_t s;
    if (queue_try_remove(&_screens, &s)) {
      since++;
      if (strcmp(s.text, last.text)) {
        _print_screen(&s, since);
        since = 0;
      }
      last = s;
    }
    int c = getchar_timeout_us(0);
    if (c == '\r' || c == '\n') {
      cmd[len] = 0;
      if (len) {
        bool scr = !strcmp(cmd, "screen");
        _command(cmd);
        if (scr) _print_screen(&last, since);
      }
      len = 0;
    } else if (c >= 0 && len < (int)sizeof(cmd) - 1) {
      cmd[len++] = c;
    }
  }
}
