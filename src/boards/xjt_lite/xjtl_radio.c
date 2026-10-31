// XJT-Lite radio bring-up: a line shell on the bay link (in on the bay's
// data pin, out on S.Port, 100000 8E2 as xjtl_test.c), the RF core with
// FrSky's PHY (70 kbps GFSK, 57 kHz deviation: cc2500.c for FrSky D/X).
//
//   y RATE DEV BW [F] PHY: symbol rate, deviation (Hz), RX bandwidth code
//                     (CC26x2 numbering), F = 1: 2-FSK, then i
//   v [WORD...]       extra RF core override words (hex), then i
//   i                 radio_init(), status
//   f KHZ             frequency
//   p WORD            TX power word (hex)
//   t N MS [LEN]      N packets of LEN bytes (30), one every MS ms
//   r MS [LEN]        receive packets of LEN bytes (30) for MS ms
//   h N MS KHZ        as t, tuned to KHZ and back before every other packet
//   o                 radio off
//
// Red LED: heartbeat, green: a packet sent or received.

#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "radio.h"
#include "timer.h"

#define BAUDRATE 100000

static radio_phy_t _phy = {
    .symbol_rate = 70020,
    .deviation = 57000,
    .rx_bw = 87,  // CC26x0 code 5, as FrSky's
    .preamble_bytes = 4,
    .sync_bits = 32,
    .if_freq = (int16_t)0x8000,
    .tx_power = 0x0CC7,  // FrSky's lowest, 9 dBm (board.h)
};
#define SYNC_WORD 0xD391D391

static uint8_t _pkt[RADIO_MAX_PKT_LEN];
static uint32_t _khz;
static radio_packet_t _rx;
static char _line[128];
static uint8_t _line_len;

static void _uart_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }
  UARTDisable(UART0_BASE);
  UARTIntDisable(UART0_BASE, 0xFFF);
  UARTIntClear(UART0_BASE, 0xFFF);
  IOCPortConfigureSet(MODULE_RX, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT | IOC_IOMODE_INV);
  IOCPortConfigureSet(SPORT_TX, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);
  UARTConfigSetExpClk(UART0_BASE, 48000000, BAUDRATE,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN);
  UARTFIFOEnable(UART0_BASE);
  UARTEnable(UART0_BASE);
}

static void _print(const char* fmt, ...)
{
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n++] = '\n';

  GPIO_clearDio(SPORT_TX_EN_N);
  for (int i = 0; i < n; i++) UARTCharPut(UART0_BASE, buf[i]);
  while (UARTBusy(UART0_BASE)) {
  }
  GPIO_setDio(SPORT_TX_EN_N);
}

static void _status(const char* what, radio_status_t st)
{
  _print("%s: %d (status 0x%04x)", what, st, radio_last_status());
}

static void _tx(uint32_t n, uint32_t period_ms, uint16_t len)
{
  uint32_t ok = 0, t0 = millis();
  for (uint32_t i = 0; i < n; i++) {
    for (uint16_t k = 0; k < len; k++) _pkt[k] = i + k;
    radio_status_t st = radio_send(_pkt, len);
    if (st == RADIO_OK) {
      ok++;
      GPIO_toggleDio(LED_GREEN);
    } else if (ok == i) {
      _status("tx", st);  // the first error only
    }
    while (millis_before(t0 + (i + 1) * period_ms)) {
    }
  }
  _print("tx: %lu/%lu sent in %lu ms", ok, n, millis() - t0);
}

// Every other packet on khz2: the synth retuned before each packet
static void _hop(uint32_t n, uint32_t period_ms, uint32_t khz2)
{
  uint32_t ok = 0, t0 = millis();
  for (uint32_t i = 0; i < n; i++) {
    radio_set_frequency(i & 1 ? khz2 : _khz);
    for (uint16_t k = 0; k < 30; k++) _pkt[k] = i + k;
    if (radio_send(_pkt, 30) == RADIO_OK) ok++;
    while (millis_before(t0 + (i + 1) * period_ms)) {
    }
  }
  radio_set_frequency(_khz);
  _print("hop: %lu/%lu sent", ok, n);
}

static void _rx_for(uint32_t ms, uint16_t len)
{
  radio_status_t st = radio_rx_start(len);
  if (st != RADIO_OK) {
    _status("rx start", st);
    return;
  }
  uint32_t n = 0, t0 = millis();
  int rssi_min = 127, rssi_max = -128;
  while (millis_before(t0 + ms)) {
    if (!radio_receive(&_rx)) continue;
    if (n == 0) _print("rx: first %02x %02x %02x %02x rssi %d", _rx.data[0], _rx.data[1],
                       _rx.data[2], _rx.data[3], _rx.rssi);
    n++;
    if (_rx.rssi < rssi_min) rssi_min = _rx.rssi;
    if (_rx.rssi > rssi_max) rssi_max = _rx.rssi;
    GPIO_toggleDio(LED_GREEN);
  }
  radio_rx_stats_t stats;
  radio_rx_stats(&stats);
  bool active = radio_rx_active();
  radio_rx_stop();
  _print("rx: %lu packets, rssi %d..%d, ok %u nok %u full %u, %s", n, rssi_min, rssi_max,
         stats.ok, stats.nok, stats.buf_full, active ? "active" : "stopped");
}

static void _command(char* line)
{
  char* arg = line + 1;
  switch (line[0]) {
    case 'i':
      _status("init", radio_init(&_phy));
      radio_set_sync_word(SYNC_WORD, SYNC_WORD);
      break;
    case 'y': {
      char* end;
      _phy.symbol_rate = strtoul(arg, &end, 10);
      _phy.deviation = strtoul(end, &end, 10);
      _phy.rx_bw = strtoul(end, &end, 10);
      _phy.fsk = strtoul(end, NULL, 10);
      _print("phy %lu %lu %u%s", _phy.symbol_rate, _phy.deviation, _phy.rx_bw,
             _phy.fsk ? " fsk" : "");
      break;
    }
    case 'v': {
      uint32_t w[RADIO_EXTRA_OVERRIDES];
      uint8_t n = 0;
      char* p = arg;
      while (n < RADIO_EXTRA_OVERRIDES) {
        char* end;
        uint32_t v = strtoul(p, &end, 16);
        if (end == p) break;
        w[n++] = v;
        p = end;
      }
      radio_set_extra_overrides(w, n);
      _print("overrides: %u", n);
      break;
    }
    case 'f': {
      _khz = strtoul(arg, NULL, 10);
      _status("freq", radio_set_frequency(_khz));
      break;
    }
    case 'p':
      _phy.tx_power = strtoul(arg, NULL, 16);
      _status("power", radio_set_tx_power(_phy.tx_power));
      break;
    case 't': {
      char* end;
      uint32_t n = strtoul(arg, &end, 10);
      uint32_t ms = strtoul(end, &end, 10);
      uint32_t len = strtoul(end, NULL, 10);
      _tx(n, ms ? ms : 10, len ? len : 30);
      break;
    }
    case 'r': {
      char* end;
      uint32_t ms = strtoul(arg, &end, 10);
      uint32_t len = strtoul(end, NULL, 10);
      _rx_for(ms ? ms : 1000, len ? len : 30);
      break;
    }
    case 'h': {
      char* end;
      uint32_t n = strtoul(arg, &end, 10);
      uint32_t ms = strtoul(end, &end, 10);
      _hop(n, ms ? ms : 10, strtoul(end, NULL, 10));
      break;
    }
    case 'o':
      radio_off();
      _print("off");
      break;
    default:
      _print("?");
  }
}

int main(void)
{
  board_init();
  timer_init();
  _uart_init();
  _print("xjtl-radio");

  uint32_t blink = millis();
  for (;;) {
    if (millis_after(blink + 500)) {
      blink += 500;
      GPIO_toggleDio(LED_RED);
    }

    while (UARTCharsAvail(UART0_BASE)) {
      int32_t c = UARTCharGetNonBlocking(UART0_BASE);
      if (c < 0 || (c & 0xF00)) continue;
      if (c == '\n' || c == '\r') {
        _line[_line_len] = 0;
        if (_line_len) _command(_line);
        _line_len = 0;
      } else if (_line_len < sizeof(_line) - 1) {
        _line[_line_len++] = c;
      }
    }
  }
}
