// "bay" bench commands on the LP-EM-CC2755P10: module bay lines without an
// external inverter or UART RX mux (the MPM v2's "Next revision",
// docs/hardware.md), the pad modes (IOC IOMODE) doing their job instead.
// LaunchPad pins:
//
//   DIO22  UART1 RXD (func 2): the radio's serial / PPM line
//   DIO21  UART1 TXD (func 2): S.Port out, open drain (the pad's pull-up
//          unless pu 0)
//   DIO20  UART0 RXD (func 2): UART0 RX moved off the XDS110's DIO2 at
//          runtime (an S.Port input on UART0), BTN1 here
//
// tools/bench/bay_test.py drives the lines from baysim (GP4 -> DIO22,
// GP5 <- DIO21). Each command blocks for MS milliseconds (the console's
// input is not read meanwhile), then the pins go back to GPIO inputs:
//
//   bay echo BAUD 8n1|8e2 RXINV od|odinv|pp|ppinv PU MS
//       UART1: each byte received on DIO22 sent back on DIO21. RXINV 1:
//       the input inverted (idle low line). od: open drain, the line idles
//       high on the pull-up; odinv: inverted open drain, the line idles low
//       (driven), the 0 bits let go; pp / ppinv: push-pull. PU 1: the pad's
//       pull-up on DIO21. "@ok bay echo rx=N err=E fe= pe= be= oe="
//   bay edges RXINV MS
//       UART1 RXD on DIO22 (100000 8E2) and the pad's edge detection on it
//       (GPIO RIS) at once: up to 128 edges timed (us), "@bay e US LEVEL"
//       ..., then "@ok bay edges n=N uart=BYTES". PPM timed from GPIO edges
//       while the UART owns the pin
//   bay u0 MS [keep]
//       UART0 RXD moved from DIO2 (XDS110) to DIO20, DIO2's input off
//       (keep: left on, both pads select UART0 RXD). Bytes received are
//       counted and the first 64 printed after: "@ok bay u0 n=N hex=..."
#include <stdlib.h>
#include <string.h>

#include <driverlib/clkctl.h>
#include <driverlib/uart.h>
#include <inc/hw_gpio.h>
#include <inc/hw_uart.h>

#include "board.h"

#define BAY_RX 22
#define BAY_TX 21
#define U0_ALT_RX 20
#define BAY_FUNC 2  // UART1 on DIO21 / DIO22, UART0 RXD on DIO20

static char* _arg(char** args)
{
  char* s = *args;
  while (*s == ' ') s++;
  if (!*s) return 0;
  char* e = s;
  while (*e && *e != ' ') e++;
  if (*e) *e++ = 0;
  *args = e;
  return s;
}

static uint32_t _num(char** args, uint32_t def)
{
  char* a = _arg(args);
  return a ? strtoul(a, 0, 0) : def;
}

static void _uart1(uint32_t baud, bool e2)
{
  CLKCTLEnable(CLKCTL_BASE, CLKCTL_UART1);
  UARTDisable(UART1_BASE);
  UARTConfigSetExpClk(UART1_BASE, BOARD_UART_CLK, baud,
                      e2 ? UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN
                         : UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
  UARTEnableFifo(UART1_BASE);
  UARTEnable(UART1_BASE);
  while (UARTCharAvailable(UART1_BASE)) (void)HWREG(UART1_BASE + UART_O_DR);
  UARTClearRxError(UART1_BASE);
}

static void _pins_off(void)
{
  while (UARTBusy(UART1_BASE)) {}
  board_ioc(BAY_RX, IOC_IOC0_PORTCFG_BASE);
  board_ioc(BAY_TX, IOC_IOC0_PORTCFG_BASE);
  UARTDisable(UART1_BASE);
}

static void _echo(char* args)
{
  uint32_t baud = _num(&args, 100000);
  char* mode = _arg(&args);
  uint32_t rxinv = _num(&args, 0);
  char* txmode = _arg(&args);
  uint32_t pu = _num(&args, 1);
  uint32_t ms = _num(&args, 1000);
  if (!mode || !txmode) {
    uart_printf("@err bay echo BAUD 8n1|8e2 RXINV od|odinv|pp|ppinv PU MS\n");
    return;
  }
  uint32_t tx_io;
  if (!strcmp(txmode, "od")) tx_io = IOC_IOC0_IOMODE_OPEND;
  else if (!strcmp(txmode, "odinv")) tx_io = IOC_IOC0_IOMODE_OPEND_INV;
  else if (!strcmp(txmode, "pp")) tx_io = IOC_IOC0_IOMODE_NORMAL;
  else if (!strcmp(txmode, "ppinv")) tx_io = IOC_IOC0_IOMODE_INVERTED;
  else {
    uart_printf("@err bay echo: tx mode %s\n", txmode);
    return;
  }

  _uart1(baud, !strcmp(mode, "8e2"));
  board_ioc(BAY_RX, BAY_FUNC | IOC_IOC0_INPEN_EN |
                        (rxinv ? IOC_IOC0_IOMODE_INVERTED : IOC_IOC0_IOMODE_NORMAL));
  board_ioc(BAY_TX, BAY_FUNC | tx_io | (pu ? IOC_IOC0_PULLCTL_PULL_UP : 0));

  uint32_t rx = 0, err = 0, fe = 0, pe = 0, be = 0, oe = 0;
  uint32_t start = board_us();
  while (board_us() - start < ms * 1000) {
    if (HWREG(UART1_BASE + UART_O_FR) & UART_FR_RXFE) continue;
    uint32_t dr = HWREG(UART1_BASE + UART_O_DR);
    rx++;
    if (dr & (UART_DR_FE | UART_DR_PE | UART_DR_BE | UART_DR_OE)) {
      err++;
      fe += !!(dr & UART_DR_FE);
      pe += !!(dr & UART_DR_PE);
      be += !!(dr & UART_DR_BE);
      oe += !!(dr & UART_DR_OE);
    }
    UARTPutChar(UART1_BASE, dr & 0xFF);
  }
  _pins_off();
  uart_printf("@ok bay echo rx=%lu err=%lu fe=%lu pe=%lu be=%lu oe=%lu\n", rx, err, fe, pe, be, oe);
}

static void _edges(char* args)
{
  uint32_t rxinv = _num(&args, 0);
  uint32_t ms = _num(&args, 1000);
  static uint32_t t[128];
  static uint8_t lvl[128];
  uint32_t n = 0, bytes = 0;

  _uart1(100000, true);
  board_ioc(BAY_RX, BAY_FUNC | IOC_IOC0_INPEN_EN | IOC_IOC0_EDGEDET_EDGE_BOTH |
                        (rxinv ? IOC_IOC0_IOMODE_INVERTED : IOC_IOC0_IOMODE_NORMAL));
  HWREG(GPIO_BASE + GPIO_O_ICLR) = 1u << BAY_RX;

  uint32_t start = board_us();
  while (board_us() - start < ms * 1000) {
    if (HWREG(GPIO_BASE + GPIO_O_RIS) & (1u << BAY_RX)) {
      uint32_t now = board_us();
      HWREG(GPIO_BASE + GPIO_O_ICLR) = 1u << BAY_RX;
      if (n < 128) {
        t[n] = now - start;
        lvl[n] = (HWREG(GPIO_BASE + GPIO_O_DIN31_0) >> BAY_RX) & 1;
      }
      n++;
    }
    while (!(HWREG(UART1_BASE + UART_O_FR) & UART_FR_RXFE)) {
      (void)HWREG(UART1_BASE + UART_O_DR);
      bytes++;
    }
  }
  _pins_off();
  HWREG(GPIO_BASE + GPIO_O_ICLR) = 1u << BAY_RX;
  for (uint32_t i = 0; i < n && i < 128; i++) uart_printf("@bay e %lu %u\n", t[i], lvl[i]);
  uart_printf("@ok bay edges n=%lu uart=%lu\n", n, bytes);
}

static void _u0(char* args)
{
  uint32_t ms = _num(&args, 1000);
  char* opt = _arg(&args);
  bool keep = opt && !strcmp(opt, "keep");
  static uint8_t buf[64];
  uint32_t n = 0;

  while (UARTBusy(UART0_BASE)) {}
  board_ioc(U0_ALT_RX, BAY_FUNC | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
  if (!keep) board_ioc(BOARD_UART_RX, IOC_IOC0_PORTCFG_BASE);
  while (UARTCharAvailable(UART0_BASE)) (void)HWREG(UART0_BASE + UART_O_DR);

  uint32_t start = board_us();
  while (board_us() - start < ms * 1000) {
    if (HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE) continue;
    uint8_t c = HWREG(UART0_BASE + UART_O_DR);
    if (n < sizeof(buf)) buf[n] = c;
    n++;
  }
  // back to the XDS110, BTN1 as before
  board_ioc(BOARD_UART_RX, BOARD_UART_IOC | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
  board_ioc(BOARD_BTN1, IOC_IOC0_PORTCFG_BASE | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
  while (UARTCharAvailable(UART0_BASE)) (void)HWREG(UART0_BASE + UART_O_DR);

  uart_printf("@ok bay u0 n=%lu hex=", n);
  for (uint32_t i = 0; i < n && i < sizeof(buf); i++) uart_printf("%02x", buf[i]);
  uart_printf("\n");
}

bool bench_port_command(const char* cmd, char* args)
{
  if (strcmp(cmd, "bay")) return false;
  char* sub = _arg(&args);
  if (sub && !strcmp(sub, "echo")) _echo(args);
  else if (sub && !strcmp(sub, "edges")) _edges(args);
  else if (sub && !strcmp(sub, "u0")) _u0(args);
  else uart_printf("@err bay echo|edges|u0\n");
  return true;
}
