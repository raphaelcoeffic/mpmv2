#include "board.h"  // IWYU pragma: keep (SX1281_NSS)
#include "sx1281.h"

#if defined(SX1281_NSS)

#include <stddef.h>

#include <driverlib/gpio.h>
#include <driverlib/ioc.h>

#include "spi.h"
#include "timer.h"

// Commands (datasheet table 12-1)
#define CMD_GET_STATUS 0xC0
#define CMD_WRITE_REGISTER 0x18
#define CMD_READ_REGISTER 0x19
#define CMD_WRITE_BUFFER 0x1A
#define CMD_READ_BUFFER 0x1B
#define CMD_SET_STANDBY 0x80
#define CMD_SET_TX 0x83
#define CMD_SET_RX 0x82
#define CMD_SET_PACKET_TYPE 0x8A
#define CMD_SET_RF_FREQUENCY 0x86
#define CMD_SET_TX_PARAMS 0x8E
#define CMD_SET_BUFFER_BASE 0x8F
#define CMD_SET_MODULATION_PARAMS 0x8B
#define CMD_SET_PACKET_PARAMS 0x8C
#define CMD_GET_RX_BUFFER_STATUS 0x17
#define CMD_GET_PACKET_STATUS 0x1D
#define CMD_SET_DIO_IRQ_PARAMS 0x8D
#define CMD_GET_IRQ_STATUS 0x15
#define CMD_CLR_IRQ_STATUS 0x97

#define CMD_CALIBRATE 0x89
#define CALIBRATE_ALL 0x3F  // RC64K, RC13M, PLL, ADC pulse, ADC bulk N and P

#define STDBY_XOSC 0x01
#define PACKET_TYPE_GFSK 0x00
#define RAMP_04_US 0x20
#define PERIOD_BASE_1MS 0x02
#define PERIOD_BASE_15US 0x00  // 15.625 us
#define CMD_SET_AUTO_FS 0x9E

#define IRQ_TX_DONE 0x0001
#define IRQ_RX_DONE 0x0002
#define IRQ_SYNC_VALID 0x0004
#define IRQ_TIMEOUT 0x4000
#define IRQ_ALL 0xFFFF

#define SYNC_SEL_1 0x10  // TX: sync word 1
#define SYNC_SEL_2 0x20  // RX: sync word 2
#define SYNC_SEL_3 0x40  // acknowledgments: sync word 3
#define FIXED_LENGTH 0x00
#define CRC_OFF 0x00
#define WHITENING_OFF 0x08

#define REG_SYNC_ERRORS 0x9CD  // bits 0-3
#define REG_SYNC_WORD_1 0x9CE  // 5 bytes each, MSB first
#define REG_SYNC_WORD_2 0x9D3
#define REG_SYNC_WORD_3 0x9D8

#define XTAL_HZ 52000000
#define BUSY_TIMEOUT_US 20000
#define TX_TIMEOUT_MS 50
#define MAX_LEN 255
#define RX_CHECK_US 1000
#define CHIP_MODE_RX 5

// GFSK bit rate / DSB bandwidth pairs (table 14-1)
static const struct {
  uint16_t kbps;
  uint16_t bw_khz;
  uint8_t code;
} _br_bw[] = {
    {2000, 2400, 0x04}, {1600, 2400, 0x28}, {1000, 1200, 0x45}, {1000, 2400, 0x4C},
    {800, 1200, 0x69},  {800, 2400, 0x70},  {500, 600, 0x86},   {500, 1200, 0x8D},
    {400, 600, 0xAA},   {400, 1200, 0xB1},  {250, 300, 0xC7},   {250, 600, 0xCE},
    {125, 300, 0xEF},
};

static bool _spi_ok;
static bool _active;
static sx1281_mod_t _mod;
static uint8_t _preamble;   // packetParam1
static uint8_t _sync_len;   // bytes
static uint32_t _tx_sync, _rx_sync;
static uint8_t _status;
static bool _rx_on;
static uint8_t _rx_len;
static uint32_t _rx_check;  // us, next check that the chip is still in RX
static sx1281_stats_t _stats;

//
// SPI
//

static bool _wait_busy()
{
  uint32_t start = micros();
  while (GPIO_readDio(SX1281_BUSY)) {
    if (micros() - start > BUSY_TIMEOUT_US) {
      _stats.busy++;
      return false;
    }
  }
  return true;
}

// One command: tx out, rx in (both len bytes, rx may be NULL)
static bool _xfer(const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  if (!_wait_busy()) return false;
  spi_select(SX1281_SPI);
  spi_transfer_8(SX1281_SPI, tx, rx, len);
  spi_unselect(SX1281_SPI);
  return true;
}

static bool _cmd(uint8_t op, const uint8_t* params, uint8_t n)
{
  uint8_t tx[1 + 16] = {op};
  for (uint8_t i = 0; i < n; i++) tx[1 + i] = params[i];
  return _xfer(tx, NULL, 1 + n);
}

#define CMD(op, ...)                                       \
  do {                                                     \
    const uint8_t _p[] = {__VA_ARGS__};                    \
    _cmd(op, _p, sizeof(_p));                              \
  } while (0)

// Read commands: opcode, NOP, then the answer (status first)
static uint16_t _read16(uint8_t op)
{
  uint8_t tx[4] = {op}, rx[4];
  _xfer(tx, rx, 4);
  _status = rx[1];
  return rx[2] << 8 | rx[3];
}

static void _write_reg(uint16_t addr, const uint8_t* data, uint8_t n)
{
  uint8_t tx[3 + 8] = {CMD_WRITE_REGISTER, addr >> 8, addr & 0xFF};
  for (uint8_t i = 0; i < n; i++) tx[3 + i] = data[i];
  _xfer(tx, NULL, 3 + n);
}

static uint8_t _read_reg(uint16_t addr)
{
  uint8_t tx[5] = {CMD_READ_REGISTER, addr >> 8, addr & 0xFF}, rx[5];
  _xfer(tx, rx, 5);
  return rx[4];
}

static void _write_buffer(const uint8_t* data, uint16_t len)
{
  if (!_wait_busy()) return;
  const uint8_t hdr[2] = {CMD_WRITE_BUFFER, 0};
  spi_select(SX1281_SPI);
  spi_write_8(SX1281_SPI, hdr, 2);
  spi_write_8(SX1281_SPI, data, len);
  spi_unselect(SX1281_SPI);
}

static void _read_buffer(uint8_t offset, uint8_t* data, uint16_t len)
{
  if (!_wait_busy()) return;
  const uint8_t hdr[3] = {CMD_READ_BUFFER, offset, 0};
  spi_select(SX1281_SPI);
  spi_write_8(SX1281_SPI, hdr, 3);
  spi_read_8(SX1281_SPI, data, len);
  spi_unselect(SX1281_SPI);
}

static void _spi_init()
{
  if (_spi_ok) return;
  // The flash shares the bus: keep it deselected
  GPIO_setDio(FLASH_CS);
  IOCPinTypeGpioOutput(FLASH_CS);
  IOCPinTypeGpioInput(SX1281_BUSY);
  IOCPinTypeGpioInput(SX1281_DIO1);
  const spi_device_t dev = {
      .frame_format = SPI_POL0_PHA0,
      .data_width = 8,
      .bit_rate = 12000000,  // 18 MHz max, 48 MHz / 4
      .rx = FLASH_MISO,
      .tx = FLASH_MOSI,
      .clk = FLASH_SCLK,
      .cs = SX1281_NSS,
  };
  spi_init(SX1281_SPI, &dev);
  _spi_ok = true;
}

//
// Front-end: U3 and the SKY66112 CTX
//

static void _front_end_sx1281()
{
  GPIO_clearDio(PA_CTX);  // RX (LNA) until a TX
  IOCPinTypeGpioOutput(PA_CTX);
  GPIO_writeDio(RF_SW, RF_SW_SX1281);
}

static void _front_end_cc2652()
{
  GPIO_writeDio(RF_SW, RF_SW_CC2652);
  IOCPortConfigureSet(PA_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);
}

//
// Setup
//

static bool _pick_mod(const radio_phy_t* phy)
{
  // Bit rate within 1%, then the narrowest bandwidth holding 2 * dev + rate
  int best = -1;
  for (unsigned i = 0; i < sizeof(_br_bw) / sizeof(_br_bw[0]); i++) {
    uint32_t rate = _br_bw[i].kbps * 1000;
    uint32_t diff = rate > phy->symbol_rate ? rate - phy->symbol_rate : phy->symbol_rate - rate;
    if (diff * 100 > rate) continue;
    if (best < 0) best = i;
    if (_br_bw[best].bw_khz * 1000 < 2 * phy->deviation + rate) best = i;
  }
  if (best < 0) return false;
  _mod.br_bw = _br_bw[best].code;
  _mod.rate = _br_bw[best].kbps * 1000;
  _mod.bw = _br_bw[best].bw_khz * 1000;

  // Modulation index 2 * dev / rate: 0.35, then 0.5 to 4 in steps of 0.25
  uint32_t h100 = 200ULL * phy->deviation / _mod.rate;
  uint8_t code = 0;
  uint32_t best_diff = h100 > 35 ? h100 - 35 : 35 - h100;
  for (uint8_t c = 1; c <= 15; c++) {
    uint32_t v = 25 + 25 * c;
    uint32_t diff = v > h100 ? v - h100 : h100 - v;
    if (diff < best_diff) best_diff = diff, code = c;
  }
  _mod.mod_ind = code;
  _mod.dev = (uint64_t)(code ? 25 + 25 * code : 35) * _mod.rate / 200;
  _mod.bt = phy->fsk ? 0x00 : 0x20;  // BT off / 0.5
  return true;
}

static void _write_sync(uint16_t reg, uint32_t sync)
{
  // Right aligned in the 5 byte register, as for the FLRC sync word
  uint8_t b[4];
  for (uint8_t i = 0; i < _sync_len; i++) b[i] = sync >> (8 * (_sync_len - 1 - i));
  _write_reg(reg + 5 - _sync_len, b, _sync_len);
}

static void _packet_params(uint8_t sync_sel, uint8_t len)
{
  CMD(CMD_SET_PACKET_PARAMS, _preamble, (_sync_len - 1) * 2, sync_sel, FIXED_LENGTH, len,
      CRC_OFF, WHITENING_OFF);
}

radio_status_t sx1281_init(const radio_phy_t* phy, int8_t tx_dbm)
{
  if (phy->sync_bits < 8 || phy->sync_bits > 32 || phy->sync_bits % 8 || !_pick_mod(phy) ||
      tx_dbm < -18 || tx_dbm > SX1281_TX_DBM_MAX)
    return RADIO_ERR_PARAM;

  _spi_init();
  _rx_on = false;
  _front_end_sx1281();
  _active = true;

  GPIO_clearDio(SX1281_NRST);
  delay_us(1000);
  GPIO_setDio(SX1281_NRST);
  delay_us(2000);
  if (!_wait_busy()) return RADIO_ERR_BOOT;

  // In STDBY_RC, as recommended at start-up and after large temperature changes
  CMD(CMD_CALIBRATE, CALIBRATE_ALL);
  if (!_wait_busy()) return RADIO_ERR_BOOT;

  uint8_t bits = phy->preamble_bytes * 8;
  if (bits > 32) bits = 32;
  _preamble = (bits / 4 - 1) << 4;
  _sync_len = phy->sync_bits / 8;

  CMD(CMD_SET_STANDBY, STDBY_XOSC);
  CMD(CMD_SET_PACKET_TYPE, PACKET_TYPE_GFSK);
  CMD(CMD_SET_BUFFER_BASE, 0, 0);
  CMD(CMD_SET_MODULATION_PARAMS, _mod.br_bw, _mod.mod_ind, _mod.bt);
  _packet_params(SYNC_SEL_1, 1);
  CMD(CMD_SET_TX_PARAMS, (uint8_t)(tx_dbm + 18), RAMP_04_US);
  // DIO1: TX or RX done, polled as a GPIO (no SPI traffic while waiting)
  CMD(CMD_SET_DIO_IRQ_PARAMS, IRQ_ALL >> 8, IRQ_ALL & 0xFF, 0, IRQ_TX_DONE | IRQ_RX_DONE, 0, 0, 0,
      0);
  sx1281_set_sync_errors(0);
  sx1281_set_sync_word(_tx_sync, _rx_sync);

  // Status bits 7-5: chip mode, 3 = STDBY_XOSC (0 or 7: nobody on the bus)
  _read16(CMD_GET_IRQ_STATUS);
  return (_status >> 5) == 3 ? RADIO_OK : RADIO_ERR_BOOT;
}

void sx1281_off()
{
  if (!_active) return;
  _rx_on = false;
  GPIO_clearDio(SX1281_NRST);
  _front_end_cc2652();
  _active = false;
}

bool sx1281_active() { return _active; }

const sx1281_mod_t* sx1281_mod() { return &_mod; }

radio_status_t sx1281_set_frequency(uint32_t freq_khz)
{
  if (!_active) return RADIO_OK;
  bool rx = _rx_on;
  if (rx) sx1281_rx_stop();
  uint32_t steps = ((uint64_t)freq_khz * 1000 << 18) / XTAL_HZ;
  CMD(CMD_SET_RF_FREQUENCY, steps >> 16, (steps >> 8) & 0xFF, steps & 0xFF);
  return rx ? sx1281_rx_start(_rx_len) : RADIO_OK;
}

void sx1281_set_sync_word(uint32_t tx_sync, uint32_t rx_sync)
{
  _tx_sync = tx_sync;
  _rx_sync = rx_sync;
  if (!_active) return;
  _write_sync(REG_SYNC_WORD_1, tx_sync);
  _write_sync(REG_SYNC_WORD_2, rx_sync);
}

void sx1281_set_sync_errors(uint8_t errors)
{
  if (!_active) return;
  uint8_t v = (_read_reg(REG_SYNC_ERRORS) & 0xF0) | (errors & 0x0F);
  _write_reg(REG_SYNC_ERRORS, &v, 1);
}

//
// TX / RX
//

// Waits for one of the IRQ flags (and returns them), 0 on time-out
static uint16_t _wait_irq(uint16_t mask, uint32_t timeout_us)
{
  uint32_t start = micros();
  while (micros() - start < timeout_us) {
    uint16_t irq = _read16(CMD_GET_IRQ_STATUS);
    if (irq & mask) return irq;
  }
  return 0;
}

static radio_status_t _send(const uint8_t* data, uint16_t len)
{
  _packet_params(SYNC_SEL_1, len);
  _write_buffer(data, len);
  CMD(CMD_CLR_IRQ_STATUS, IRQ_ALL >> 8, IRQ_ALL & 0xFF);
  GPIO_setDio(PA_CTX);
  CMD(CMD_SET_TX, PERIOD_BASE_1MS, 0, TX_TIMEOUT_MS);

  // TX done on DIO1 (the TX time-out of the chip ends it too, without DIO1)
  uint32_t start = micros();
  while (!GPIO_readDio(SX1281_DIO1)) {
    if (micros() - start > TX_TIMEOUT_MS * 1000) {
      GPIO_clearDio(PA_CTX);
      _stats.tx_fail++;
      _stats.last_status = sx1281_last_status();
      return RADIO_ERR_TIMEOUT;
    }
  }
  GPIO_clearDio(PA_CTX);
  return RADIO_OK;
}

radio_status_t sx1281_send(const uint8_t* data, uint16_t len)
{
  if (!_active || len == 0 || len > MAX_LEN) return RADIO_ERR_PARAM;
  if (_rx_on) sx1281_rx_stop();
  return _send(data, len);
}

static void _read_packet(radio_packet_t* pkt, uint8_t len)
{
  pkt->timestamp_us = micros();
  uint8_t offset = _read16(CMD_GET_RX_BUFFER_STATUS) & 0xFF;
  _read_buffer(offset, pkt->data, len);
  pkt->len = len;

  uint8_t tx[7] = {CMD_GET_PACKET_STATUS}, rx[7];
  _xfer(tx, rx, 7);
  pkt->rssi = -(int)rx[3] / 2;  // rssiSync
  pkt->freq_offset = RADIO_FREQ_OFFSET_NONE;
}

radio_status_t sx1281_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                              uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt)
{
  if (!_active || len == 0 || len > MAX_LEN || rx_len == 0 || rx_len > MAX_LEN)
    return RADIO_ERR_PARAM;
  if (_rx_on) sx1281_rx_stop();
  pkt->len = 0;

  // Synthesizer kept on after TX (AutoFs): RX starts sooner
  _write_sync(REG_SYNC_WORD_3, rx_sync);
  CMD(CMD_SET_AUTO_FS, 1);
  uint16_t steps = (window_us * 64 + 999) / 1000;  // 15.625 us steps
  radio_status_t st = _send(data, len);
  uint32_t t0 = micros();
  if (st == RADIO_OK) {
    // As few commands as possible before RX: the NRF answers 130 us after
    // the end of the packet (TX done flag left set, not waited for)
    _packet_params(SYNC_SEL_3, rx_len);
    CMD(CMD_SET_RX, PERIOD_BASE_15US, steps >> 8, steps & 0xFF);
    _wait_busy();  // low once RX is ready
    uint32_t turn = micros() - t0;
    if (turn > _stats.turn_max) _stats.turn_max = turn;
    uint16_t irq = _wait_irq(IRQ_RX_DONE | IRQ_TIMEOUT, window_us + 5000);
    if (irq & IRQ_RX_DONE) {
      _stats.ack_rx++;
      _read_packet(pkt, rx_len);
    } else if (irq) {
      _stats.ack_timeout++;
      st = RADIO_NO_PACKET;
    } else {
      _stats.ack_none++;
      _stats.last_status = _status;
      st = RADIO_ERR_TIMEOUT;
    }
  }
  CMD(CMD_SET_AUTO_FS, 0);
  CMD(CMD_SET_STANDBY, STDBY_XOSC);
  return st;
}

static void _rx_arm()
{
  CMD(CMD_CLR_IRQ_STATUS, IRQ_ALL >> 8, IRQ_ALL & 0xFF);
  CMD(CMD_SET_RX, PERIOD_BASE_1MS, 0, 0);  // single mode, no timeout
}

radio_status_t sx1281_rx_start(uint16_t len)
{
  if (!_active || len == 0 || len > MAX_LEN) return RADIO_ERR_PARAM;
  sx1281_clear_stats();
  _rx_check = micros() + RX_CHECK_US;
  _rx_len = len;
  _packet_params(SYNC_SEL_2, len);
  _rx_arm();
  _rx_on = true;
  return RADIO_OK;
}

void sx1281_rx_stop()
{
  if (!_rx_on) return;
  CMD(CMD_SET_STANDBY, STDBY_XOSC);
  _rx_on = false;
}

bool sx1281_receive(radio_packet_t* pkt)
{
  if (!_rx_on) return false;
  uint16_t irq = 0;
  if (GPIO_readDio(SX1281_DIO1)) irq = _read16(CMD_GET_IRQ_STATUS);
  else if (micros_before(_rx_check)) return false;
  else irq = _read16(CMD_GET_IRQ_STATUS);  // updates _status for the check below
  if (!(irq & IRQ_RX_DONE)) {
    // Out of RX without a packet (or not answering): arm again
    if (micros_after(_rx_check)) {
      _rx_check = micros() + RX_CHECK_US;
      if ((_status >> 5) != CHIP_MODE_RX) {
        _stats.rearm++;
        _stats.last_status = _status;
        _stats.last_irq = irq;
        _rx_arm();
      }
    }
    return false;
  }
  _stats.ok++;
  _read_packet(pkt, _rx_len);

  _rx_arm();
  return true;
}

void sx1281_rx_stats(sx1281_stats_t* stats) { *stats = _stats; }

void sx1281_clear_stats() { _stats = (sx1281_stats_t){0}; }

uint16_t sx1281_last_status()
{
  if (!_active) return 0;
  uint8_t tx[1] = {CMD_GET_STATUS}, rx[1];
  _xfer(tx, rx, 1);
  return rx[0];
}

#else  // no SX1281 on this board

radio_status_t sx1281_init(const radio_phy_t* phy, int8_t tx_dbm)
{
  (void)phy, (void)tx_dbm;
  return RADIO_ERR_PARAM;
}
void sx1281_off() {}
bool sx1281_active() { return false; }
const sx1281_mod_t* sx1281_mod()
{
  static const sx1281_mod_t none;
  return &none;
}
radio_status_t sx1281_set_frequency(uint32_t freq_khz) { (void)freq_khz; return RADIO_OK; }
void sx1281_set_sync_word(uint32_t tx_sync, uint32_t rx_sync) { (void)tx_sync, (void)rx_sync; }
void sx1281_set_sync_errors(uint8_t errors) { (void)errors; }
radio_status_t sx1281_send(const uint8_t* data, uint16_t len)
{
  (void)data, (void)len;
  return RADIO_ERR_PARAM;
}
radio_status_t sx1281_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                              uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt)
{
  (void)data, (void)len, (void)rx_sync, (void)rx_len, (void)window_us, (void)pkt;
  return RADIO_ERR_PARAM;
}
radio_status_t sx1281_rx_start(uint16_t len) { (void)len; return RADIO_ERR_PARAM; }
void sx1281_rx_stop() {}
bool sx1281_receive(radio_packet_t* pkt) { (void)pkt; return false; }
void sx1281_rx_stats(sx1281_stats_t* stats) { *stats = (sx1281_stats_t){0}; }
void sx1281_clear_stats() {}
uint16_t sx1281_last_status() { return 0; }

#endif
