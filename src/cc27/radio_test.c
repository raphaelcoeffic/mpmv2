// CC2755P10 LaunchPad radio bring-up: a line shell on UART0 (XDS110 back
// channel, 115200 8N1) driving the radio through rf.c, raw framing on TI's
// NESB PHY (2-GFSK, 1 or 2 Mbps).
//
//   i [1|2|3|4]       PHY setup: NESB 1 or 2 Mbps (default 1), 3 TI's BLE
//                     1 Mbps, 4 GFSK 250 kbps (TI's CC23xx MSK
//                     PHY, ported, TX as GFSK)
//   s HEX [BITS]      sync word, MSB first as on air (default 32 bits)
//   f KHZ             frequency (default 2440000)
//   p DBM             TX power, from the PA table (default 0)
//   t N MS [LEN]      N packets of LEN bytes (16), one every MS ms; byte 0
//                     counts
//   r MS [LEN]        receive packets of LEN bytes (16) for MS ms
//   q MS [LEN]        the same, counted (byte 0 a counter): received, missed,
//                     bad, RSSI min/mean/max
//   x                 last end cause, events, RX counters
//   d 0|1             RX debugging: events and RSSI during the window
//   v                 the cores' RAM against the PHY's images
//   b                 PBE RAM variables
//
// Red LED: heartbeat, green: a packet sent or received.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_fcfg.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)

#include "board.h"
#include "phy_ble.h"
#include "phy_fsk.h"
#include "phy_fsk250.h"
#include "phy_nesb.h"
#include "rf.h"

static uint32_t _khz = 2440000;
static uint32_t _sync = 0x71764129;
static uint8_t _sync_bits = 32;
static uint8_t _pkt[RF_MAX_PAYLOAD];
static char _line[96];
static uint8_t _line_len;

// 1, 2: NESB 1 or 2 Mbps; 3: TI's generic BLE 1 Mbps PHY (same images);
// 4: GFSK 250 kbps (genfsk images)
static const LRF_Config *_phy;

static void _setup(int phy)
{
  static const LRF_Config *const phys[] = {&phy_nesb_1_mbps, &phy_nesb_2_mbps, &phy_ble_1_mbps,
                                           &phy_fsk250_common};
  if (phy < 1 || phy > 4) phy = 1;
  _phy = phys[phy - 1];
  int res = rf_configure(_phy);
  rf_set_sync(_sync, _sync_bits);
  uart_printf("@setup %d res=%d\n", phy, res);
}

// GFSK at any rate (phy_fsk)
static void _setup_fsk(uint32_t rate, uint32_t dev, int log2_pdif)
{
  phy_fsk_t phy = {.rate = rate, .deviation = dev, .log2_pdif = log2_pdif};
  phy_fsk_info_t info;
  int res = phy_fsk_setup(&phy, &info);
  _phy = rate > 1200000 ? &phy_nesb_2_mbps : &phy_fsk250_common;
  rf_set_sync(_sync, _sync_bits);
  uart_printf("@fsk rate=%lu dev=%lu res=%d pre=%u rw=0x%lx bde=%u/%u pdif=%u demmisc3=0x%04x\n",
              info.rate, dev, res, info.prescaler, info.rate_word, info.bde1, info.bde2, info.pdif,
              info.demmisc3);
}

static void _tx(int n, int ms, int len)
{
  int ok = 0;
  uint32_t t0 = board_us();
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < len; j++) _pkt[j] = j;
    _pkt[0] = i;
    int res = rf_tx(_khz * 1000, _pkt, len);
    if (res == RF_OK) {
      ok++;
      board_led_toggle(BOARD_LED_GREEN);
    } else {
      uart_printf("@tx %d res=%d end=0x%02x ev=0x%08lx\n", i, res, rf_last_endcause(), rf_last_events());
    }
    while (board_us() - t0 < (uint32_t)(i + 1) * ms * 1000) {
    }
  }
  uart_printf("@tx done %d/%d\n", ok, n);
}

static void _rx(int ms, int len)
{
  rf_packet_t pkt;
  uint32_t t0 = board_us();
  int n = 0;
  while (board_us() - t0 < (uint32_t)ms * 1000) {
    uint32_t left = ms * 1000 - (board_us() - t0);
    int res = rf_rx(_khz * 1000, len, left, &pkt);
    if (res == RF_OK) {
      n++;
      board_led_toggle(BOARD_LED_GREEN);
      char hex[2 * RF_MAX_PAYLOAD + 1];
      for (int i = 0; i < pkt.len; i++) sprintf(hex + 2 * i, "%02x", pkt.data[i]);
      hex[2 * pkt.len] = 0;
      uart_printf("@rx %lu %s rssi=%d\n", (board_us() - t0) / 1000, hex, pkt.rssi);
    } else if (res != RF_ERR_NO_PACKET) {
      uart_printf("@rx res=%d end=0x%02x ev=0x%08lx\n", res, rf_last_endcause(), rf_last_events());
      break;
    }
  }
  uart_printf("@rx done %d\n", n);
}

// Packets from a transmitter counting in byte 0 (MPM bench txp): received,
// missed (counter gaps), bad (other bytes unlike the first packet's), RSSI
static void _rx_count(int ms, int len)
{
  rf_packet_t pkt;
  uint8_t ref[RF_MAX_PAYLOAD];
  int n = 0, missed = 0, bad = 0, errs = 0, rssi_sum = 0;
  int rssi_min = 127, rssi_max = -128;
  uint8_t last = 0;
  uint32_t t0 = board_us();
  while (board_us() - t0 < (uint32_t)ms * 1000) {
    uint32_t left = ms * 1000 - (board_us() - t0);
    int res = rf_rx(_khz * 1000, len, left, &pkt);
    if (res == RF_OK && pkt.len == len) {
      if (n == 0) {
        memcpy(ref, pkt.data, len);
      } else {
        missed += (uint8_t)(pkt.data[0] - last - 1);
        if (memcmp(ref + 1, pkt.data + 1, len - 1)) bad++;
      }
      last = pkt.data[0];
      n++;
      rssi_sum += pkt.rssi;
      if (pkt.rssi < rssi_min) rssi_min = pkt.rssi;
      if (pkt.rssi > rssi_max) rssi_max = pkt.rssi;
    } else if (res != RF_ERR_NO_PACKET) {
      errs++;
    }
  }
  uart_printf("@count n=%d missed=%d bad=%d errors=%d rssi=%d/%d/%d\n", n, missed, bad, errs,
              rssi_min, n ? rssi_sum / n : 0, rssi_max);
}

#define R32(a) (*(volatile uint32_t *)(a))
#define R16(a) (*(volatile uint16_t *)(a))

static void _debug(int step, uint32_t events, int8_t rssi, uint16_t endcause)
{
  uart_printf("@dbg %d ev=0x%08lx rssi=%d max=%d mdm=%lx end=0x%02x\n", step, events, rssi,
              (int8_t)R32(0x40083230), R32(0x40082000), endcause);
  // MDM: DEMFIDC1/2, DEMSOFTPDIFF, LQIEST, DSCAPT0-3, EVT1
  uart_printf("@dem fidc=%lx/%lx soft=%lx lqi=%lx capt=%lx %lx %lx %lx evt1=%lx\n",
              R32(0x40082168), R32(0x4008216c), R32(0x40082180), R32(0x400820b0),
              R32(0x40082234), R32(0x40082238), R32(0x4008223c), R32(0x40082240),
              R32(0x4008202c));
  if (step == 0 || step == 7) {
    // PBE: ENABLE, API, MDMAPI, MDMMSGBOX, RFEAPI, RFEMSGBOX; MDM, RFE:
    // ENABLE; RFE API, MSGBOX, SPARE4; BUFRAM MSGBOX, OPCFG
    uart_printf("@pbe en=%lx api=%lx mdmapi=%lx mdmmsg=%lx rfeapi=%lx rfemsg=%lx\n",
                R32(0x40081000), R32(0x40081030), R32(0x40081044), R32(0x40081048),
                R32(0x40081064), R32(0x40081070));
    uart_printf("@cores mdm=%lx rfe=%lx rfeapi=%lx rfemsg=%lx spare4=%lx msgbox=%x opcfg=%x maxlen=%u\n",
                R32(0x40082000), R32(0x40083000), R32(0x40083048), R32(0x40083054),
                R32(0x400830ac), R16(0x40092004), R16(0x40092042), R16(0x40092044));
    // RFE: HFXTSTAT, RFSTATE, EVT0/1, RXCTRL, IFADCSTAT, RFGAIN, MCECMDIN,
    // PBECMDIN; MDM: DEMENABLE0/1, EVT0/1, RFECMDOUT/IN
    uart_printf("@rfe hfxt=%lx state=%lx evt=%lx/%lx rxctrl=%lx adcstat=%lx gain=%lx mcecmd=%lx pbecmd=%lx\n",
                R32(0x40083028), R32(0x40083030), R32(0x40083010), R32(0x40083014),
                R32(0x40083220), R32(0x40083238), R32(0x40083234), R32(0x40083064),
                R32(0x40083078));
    uart_printf("@mdm dem=%lx/%lx evt=%lx/%lx rfecmd=%lx/%lx\n", R32(0x40082010),
                R32(0x40082014), R32(0x40082028), R32(0x4008202c), R32(0x40082098),
                R32(0x4008209c));
    // MDM: API, MSGBOX, CMDPAR0-2, EVT0/2, PBECMDOUT/IN, FREQ
    uart_printf("@mce api=%lx msg=%lx par=%lx/%lx/%lx evt0=%lx evt2=%lx pbecmd=%lx/%lx freq=%lx\n",
                R32(0x4008205c), R32(0x4008206c), R32(0x40082060), R32(0x40082064),
                R32(0x40082068), R32(0x40082028), R32(0x40082030), R32(0x400820a8),
                R32(0x400820ac), R32(0x40082070));
    // RFE: ANAISOCTL, IFADC0/1, DIVCTL, LNA, MAGNACC0/1; MDM ADCDIGCONF
    uart_printf("@ana iso=%lx ifadc=%lx/%lx divctl=%lx lna=%lx acc=%lx/%lx adcdig=%lx\n",
                R32(0x40083218), R32(0x400830c4), R32(0x400830c8), R32(0x4008321c),
                R32(0x400830b4), R32(0x40083224), R32(0x40083228), R32(0x400820c0));
    // MDM: SYNC0-3, DEMSWQU0, DEMC1BE0/1, DEMC1BEREF0-3
    uart_printf("@sync %lx %lx %lx %lx swqu=%lx c1be=%lx/%lx ref=%lx %lx %lx %lx\n",
                R32(0x4008225c), R32(0x40082260), R32(0x40082264), R32(0x40082268),
                R32(0x40082134), R32(0x40082118), R32(0x4008211c), R32(0x40082138),
                R32(0x4008213c), R32(0x40082140), R32(0x40082144));
    // RFE: SPARE0-5, RSSIOFFSET, MAGNTHRCFG, MAGNTHR, GAINCTL, MAGNCTL0/1
    uart_printf("@agc spare=%lx/%lx/%lx/%lx/%lx/%lx rssioff=%lx thr=%lx/%lx gainctl=%lx magn=%lx/%lx\n",
                R32(0x4008309c), R32(0x400830a0), R32(0x400830a4), R32(0x400830a8),
                R32(0x400830ac), R32(0x400830b0), R32(0x4008308c), R32(0x40083084),
                R32(0x40083088), R32(0x40083090), R32(0x40083094), R32(0x40083098));
  }
}

static void _verify(const char *name, const LRF_TOPsmImage *image, uint32_t base)
{
  int bad = 0;
  for (uint32_t i = 0; i < image->imageLen; i++) {
    uint32_t w = R16(base + 4 * i) | (uint32_t)R16(base + 4 * i + 2) << 16;
    if (w != image->image[i] && bad++ < 4)
      uart_printf("@verify %s [%lu] %08lx != %08lx\n", name, i, w, image->image[i]);
  }
  uart_printf("@verify %s %lu words, %d differ\n", name, image->imageLen, bad);
}

static void _command(char *line)
{
  char *argv[6];
  int argc = 0;
  for (char *tok = strtok(line, " "); tok && argc < 6; tok = strtok(NULL, " ")) argv[argc++] = tok;
  if (argc == 0) return;
  long a1 = argc > 1 ? strtol(argv[1], NULL, 0) : 0;
  long a2 = argc > 2 ? strtol(argv[2], NULL, 0) : 0;
  long a3 = argc > 3 ? strtol(argv[3], NULL, 0) : 0;

  switch (argv[0][0]) {
    case 'i':
      _setup(argc > 1 ? a1 : 1);
      break;
    case 'g':  // g RATE DEV [LOG2_PDIF]: GFSK at any rate
      _setup_fsk(a1, a2, argc > 3 ? a3 : -1);
      break;
    case 's':
      _sync = strtoul(argv[1], NULL, 16);
      _sync_bits = argc > 2 ? a2 : 32;
      rf_set_sync(_sync, _sync_bits);
      uart_printf("@sync %s\n", argv[1]);
      break;
    case 'f':
      _khz = a1;
      uart_printf("@freq %lu\n", _khz);
      break;
    case 'p':
      rf_set_power(a1);
      uart_printf("@power %ld\n", a1);
      break;
    case 't':
      _tx(a1, a2, argc > 3 ? a3 : 16);
      break;
    case 'r':
      _rx(a1, argc > 2 ? a2 : 16);
      break;
    case 'q':
      _rx_count(a1, argc > 2 ? a2 : 16);
      break;
    case 'd':
      rf_debug = a1 ? _debug : NULL;
      uart_printf("@debug %ld\n", a1);
      break;
    case 'x': {
      rf_stats_t st;
      rf_stats(&st);
      uart_printf("@status end=0x%02x ev=0x%08lx ok=%u nok=%u ign=%u empty=%u full=%u rssi=%d\n",
                  rf_last_endcause(), rf_last_events(), st.rx_ok, st.rx_nok, st.rx_ignored,
                  st.rx_empty, st.rx_fifo_full, st.last_rssi);
      break;
    }
    case 'w':  // w ADDR VAL: 32-bit register write (radio setup writes undo it)
      R32(strtoul(argv[1], NULL, 0)) = strtoul(argv[2], NULL, 0);
      // fall through
    case 'm':  // m ADDR: 32-bit register read
      uart_printf("@mem %s = %lx\n", argv[1], R32(strtoul(argv[1], NULL, 0)));
      break;
    case 'a': {  // FCFG application trims (what LRF_setupRadio applies)
      const uint32_t *t = (const uint32_t *)&((const fcfg_t *)FCFG_BASE)->appTrims;
      uart_printf("@trims %p rev=%u\n", t, *(const uint8_t *)t);
      for (int i = 0; i < 32; i += 8)
        uart_printf("@trims %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", t[i], t[i + 1],
                    t[i + 2], t[i + 3], t[i + 4], t[i + 5], t[i + 6], t[i + 7]);
      uart_printf("@iqmc %lx ifadc %lx/%lx/%lx quant %lx\n", R32(0x400820f0), R32(0x400830c4),
                  R32(0x400830c8), R32(0x400830cc), R32(0x400830d0));
      break;
    }
    case 'z':  // system reset
      uart_printf("@reset\n");
      for (uint32_t t = board_us(); board_us() - t < 2000;) {
      }
      R32(0xE000ED0C) = 0x05FA0004;  // AIRCR: SYSRESETREQ
      break;
    case 'v':  // cores' RAM against the PHY's images (16-bit reads)
      _verify("pbe", _phy->pbeImage, LRFD_PBERAM_BASE);
      _verify("mce", _phy->mceImage, LRFD_MCERAM_BASE);
      _verify("rfe", _phy->rfeImage, LRFD_RFERAM_BASE);
      break;
    case 'b':  // PBE RAM variables, 0x20 to 0xBF (16-bit)
      for (uint32_t o = 0x20; o < 0xC0; o += 16)
        uart_printf("@pbe %02lx: %04x %04x %04x %04x %04x %04x %04x %04x\n", o,
                    R16(LRFD_BUFRAM_BASE + o), R16(LRFD_BUFRAM_BASE + o + 2),
                    R16(LRFD_BUFRAM_BASE + o + 4), R16(LRFD_BUFRAM_BASE + o + 6),
                    R16(LRFD_BUFRAM_BASE + o + 8), R16(LRFD_BUFRAM_BASE + o + 10),
                    R16(LRFD_BUFRAM_BASE + o + 12), R16(LRFD_BUFRAM_BASE + o + 14));
      break;
    case 'h':  // h ADDR [VAL]: 16-bit read, or write then read (LRF RAM)
      if (argc > 2) R16(strtoul(argv[1], NULL, 0)) = strtoul(argv[2], NULL, 0);
      uart_printf("@mem %s = %x\n", argv[1], R16(strtoul(argv[1], NULL, 0)));
      break;
    default:
      uart_printf("@error %s\n", argv[0]);
  }
}

int main(void)
{
  board_init();
  board_uart_init(115200);
  bool hfxt = board_hfxt_wait(10000);
  rf_init();
  uart_printf("\n@cc27 radio hfxt=%d\n", hfxt);
  _setup(1);

  uint32_t last = board_us();
  for (;;) {
    int c = uart_getc();
    if (c == '\r' || c == '\n') {
      _line[_line_len] = 0;
      if (_line_len) _command(_line);
      _line_len = 0;
    } else if (c >= 0 && _line_len < sizeof(_line) - 1) {
      _line[_line_len++] = c;
    }
    if (board_us() - last > 500000) {
      last += 500000;
      board_led_toggle(BOARD_LED_RED);
    }
  }
}
