// CC2500_SPI.ino of the MULTI-Module on the CC2652: a CC2500 behind its
// registers. The protocols write the registers and strobe as on the chip; at
// STX and SRX the modem, channel and packet registers become the RF core
// setup (re-initialized only when the PHY changes) and frame.c's CC2500
// framing. TX sends the TX FIFO; RX fills a 64 byte RX FIFO as the chip does
// (length byte, payload, RSSI and LQI | CRC_OK with APPEND_STATUS), address
// check included. Read as the MPM reads it: RXBYTES, MARCSTATE, RSSI.
// The RF core gives whole packets and starts sending about 0.2 ms later than
// the CC2500 after the strobe: RXBYTES waits for a packet that ends within
// RX_WAIT_US, which the CC2500 would have in its FIFO by then (FrSky X LBT
// telemetry, received just before the protocol reads it).
//
// Not emulated: SYNC_MODE 0 / 4 (no preamble or sync word: CC2500_250K_Init(),
// the XN297 at 250 kbps runs on xn297.c), infinite packet length, TXOFF_MODE
// and RXOFF_MODE other than IDLE / staying in RX, OOK, Manchester, wake on
// radio, the GDO pins. Calibration strobes do nothing (the RF core calibrates
// at each tune), FSCAL1 reads back what was written
#include "iface_cc2500.h"

#include "board.h"
#include "frame.h"
#include "radio.h"
#include "timer.h"

#define FXOSC 26000000ULL
#define FIFO_SIZE 64
#define RX_WAIT_US 500
#define RSSI_OFFSET 72  // dBm, CC2500 datasheet table 31 (250 kBaud: 72, 2.4: 71)

// Reset values (datasheet section 33), 0x00 to 0x2E
static const uint8_t _reset_regs[] = {
    0x29, 0x2E, 0x3F, 0x07, 0xD3, 0x91, 0xFF, 0x04, 0x45, 0x00, 0x00, 0x0F, 0x00, 0x5E, 0xC4, 0xEC,
    0x8C, 0x22, 0x02, 0x22, 0xF8, 0x47, 0x07, 0x30, 0x04, 0x36, 0x6C, 0x03, 0x40, 0x91, 0x87, 0x6B,
    0xF8, 0x56, 0x10, 0xA9, 0x0A, 0x20, 0x0D, 0x41, 0x00, 0x59, 0x7F, 0x3F, 0x88, 0x31, 0x0B};
#define NREGS sizeof(_reset_regs)

// MARCSTATE values
#define MARC_IDLE 0x01
#define MARC_RX 0x0D
#define MARC_TX 0x13

// RF core RX bandwidth codes (kHz). As suite_cc2500.py --phy, tested with
// the MPM's CC2500 setups: the narrowest above 2 x deviation + rate + 70 kHz,
// 89 for MSK
static const struct {
  uint8_t code;
  uint16_t khz;
} _rx_bw[] = {{85, 174}, {86, 220}, {87, 264}, {88, 307}, {89, 349}, {90, 440},
              {91, 529}, {92, 614}, {94, 882}, {97, 1399}, {100, 2458}};

static uint8_t _reg[NREGS];
static uint8_t _patable;
static enum { S_IDLE, S_RX, S_TX } _state;

static uint8_t _tx_fifo[FIFO_SIZE], _tx_len;
static uint8_t _rx_fifo[FIFO_SIZE], _rx_len;
static uint8_t _lqi;

static frame_t _frame;
static radio_phy_t _phy;
static bool _phy_on;
static uint32_t _freq_khz;
static uint16_t _power;
static uint8_t _rx_max;  // RX: largest payload (length byte included)
static volatile bool _rx_busy;       // a packet's length came, not the packet yet
static volatile uint32_t _rx_end_us;  // its expected end
static radio_packet_t _pkt;
static uint8_t _air[FRAME_AIR_MAX];

static uint32_t _pow2(uint8_t e) { return 1UL << e; }

// TX power word for a PATABLE value: the MPM's high and LBT power (+1 and
// -4 dBm on the CC2500) at the board default, low power (-6 dBm) reduced,
// bind and range (-30 dBm) low
static uint16_t _power_word(uint8_t pa)
{
  switch (pa) {
    case CC2500_POWER_17: case CC2500_POWER_16: case CC2500_POWER_15: case CC2500_POWER_14:
      return RF_TX_POWER_DEFAULT;
    case CC2500_POWER_13: case CC2500_POWER_12: case CC2500_POWER_11:
      return RF_TX_POWER_REDUCED;
    default:
      return RF_TX_POWER_LOW;
  }
}

static uint32_t _rate()
{
  return ((256 + _reg[CC2500_11_MDMCFG3]) * FXOSC * _pow2(_reg[CC2500_10_MDMCFG4] & 0x0F)) >> 28;
}

// PHY and framing from the registers: false if not supported
static bool _setup(radio_phy_t* phy, frame_t* f)
{
  uint8_t mdmcfg2 = _reg[CC2500_12_MDMCFG2], mdmcfg1 = _reg[CC2500_13_MDMCFG1];
  uint8_t pktctrl0 = _reg[CC2500_08_PKTCTRL0];
  uint8_t mod = (mdmcfg2 >> 4) & 7, sync_mode = mdmcfg2 & 3;
  if (sync_mode == 0 || (mod != 0 && mod != 1 && mod != 7) || (pktctrl0 & 3) > 1) return false;

  uint32_t rate = _rate();
  uint8_t dev = _reg[CC2500_15_DEVIATN];
  uint32_t deviation = (FXOSC * (8 + (dev & 7)) * _pow2((dev >> 4) & 7)) >> 17;
  // MSK as 2-FSK, bits inverted. Full phase step (DEVIATN M = 7): rate / 4,
  // partial: 0.32 x rate (the bench presets, Hitec / Redpine / Skyartec)
  if (mod == 7) deviation = (dev & 7) == 7 ? rate / 4 : rate * 32 / 100;
  uint32_t need_khz = (2 * deviation + rate) / 1000 + 70;
  uint8_t bw = _rx_bw[sizeof(_rx_bw) / sizeof(_rx_bw[0]) - 1].code;
  for (int i = sizeof(_rx_bw) / sizeof(_rx_bw[0]) - 1; i >= 0; i--)
    if (_rx_bw[i].khz >= need_khz) bw = _rx_bw[i].code;
  if (mod == 7) bw = 89;
  static const uint8_t preamble[] = {2, 3, 4, 6, 8, 12, 16, 24};
  uint8_t pre = preamble[(mdmcfg1 >> 4) & 7];

  memset(f, 0, sizeof(*f));
  f->type = FRAME_CC2500;
  f->addr[0] = f->addr[2] = _reg[CC2500_04_SYNC1];
  f->addr[1] = f->addr[3] = _reg[CC2500_05_SYNC0];
  f->addr_len = sync_mode == 3 ? 4 : 2;
  f->cc2400_crc = pktctrl0 & 0x08;
  f->crc_len = pktctrl0 & 0x04 || f->cc2400_crc ? 2 : 0;
  f->var_len = pktctrl0 & 1;
  f->whitening = pktctrl0 & 0x40;
  f->fec = mdmcfg1 & 0x80;
  f->invert = mod == 7;
  if (!frame_init(f)) return false;

  // Field by field: _prepare() compares the structs
  memset(phy, 0, sizeof(*phy));
  phy->symbol_rate = rate;
  phy->deviation = (deviation + 125) / 250 * 250;
  phy->rx_bw = bw;
  phy->preamble_bytes = pre;  // and frame.c's 2 (as the bench)
  phy->sync_bits = frame_sync_bits(f);
  phy->if_freq = (int16_t)0x8000;
  phy->tx_power = _power;
  phy->sync_threshold = frame_rx_sync_weak(f) ? 0x32 : 0;
  phy->fsk = mod != 1;
  return true;
}

static uint32_t _channel_khz()
{
  uint32_t freq = (uint32_t)_reg[CC2500_0D_FREQ2] << 16 | _reg[CC2500_0E_FREQ1] << 8 |
                  _reg[CC2500_0F_FREQ0];
  uint64_t hz = freq * FXOSC >> 16;
  uint8_t chanspc_e = _reg[CC2500_13_MDMCFG1] & 3;
  hz += (uint64_t)_reg[CC2500_0A_CHANNR] *
        (((256 + _reg[CC2500_14_MDMCFG0]) * FXOSC * _pow2(chanspc_e)) >> 18);
  int64_t offset = (int8_t)_reg[CC2500_0C_FSCTRL0] * (int64_t)FXOSC >> 14;
  return (uint32_t)((int64_t)hz + offset + 500) / 1000;
}

// The registers of the last _setup(), the frequency ones left out
static uint8_t _setup_regs[NREGS];

static bool _regs_changed()
{
  for (uint8_t i = 0; i < NREGS; i++) {
    if (i == CC2500_0A_CHANNR || i == CC2500_0C_FSCTRL0 ||
        (i >= CC2500_0D_FREQ2 && i <= CC2500_0F_FREQ0) ||
        (i >= CC2500_23_FSCAL3 && i <= CC2500_26_FSCAL0))
      continue;
    if (_reg[i] != _setup_regs[i]) return true;
  }
  return false;
}

// Radio set up for the registers: false if not supported (nothing sent).
// _setup() and frame_init() (about 170 us) only after a register change
static bool _prepare()
{
  if (!_phy_on || _regs_changed()) {
    radio_phy_t phy;
    frame_t f;
    if (!_setup(&phy, &f)) return false;
    if (!_phy_on || memcmp(&phy, &_phy, sizeof(phy))) {
      _phy = phy;
      _freq_khz = 0;
      if (radio_init(&_phy) != RADIO_OK) {
        _phy_on = false;
        return false;
      }
      _phy_on = true;
    }
    _frame = f;
    memcpy(_setup_regs, _reg, NREGS);
  }
  radio_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
  uint32_t khz = _channel_khz();
  if (khz != _freq_khz) {
    _freq_khz = khz;
    radio_set_frequency(khz);
  }
  return true;
}

static void _tx()
{
  if (_state == S_RX) radio_rx_stop();
  _rx_busy = false;
  _state = S_IDLE;
  if (!_prepare()) return;
  uint8_t len = _tx_len;
  if (_frame.var_len) {
    if (!len || _tx_fifo[0] + 1 > len) return;  // TX FIFO underflow
    len = _tx_fifo[0] + 1;
  } else {
    if (_reg[CC2500_06_PKTLEN] > len) return;
    len = _reg[CC2500_06_PKTLEN];
  }
  uint16_t n = frame_tx_data(&_frame, _tx_fifo, len, _air);
  if (!n) return;
  radio_tx_start(_air, n);
  _state = S_TX;
  _tx_len = 0;
}

static uint16_t _rx_var_len(const uint8_t* data, uint16_t n, void* ctx)
{
  (void)ctx;
  uint16_t len = frame_rx_var_len(&_frame, _rx_max, data, n);
  if (len > n) {
    _rx_end_us = micros() + (uint32_t)(len - n) * 8000000 / _phy.symbol_rate;
    _rx_busy = true;
  }
  return len;
}

static void _rx()
{
  if (_state == S_RX) radio_rx_stop();
  _rx_busy = false;
  _state = S_IDLE;
  if (!_prepare()) return;
  _rx_max = _frame.var_len ? _reg[CC2500_06_PKTLEN] + 1 : _reg[CC2500_06_PKTLEN];
  if (_rx_max > FRAME_PAYLOAD_MAX) _rx_max = FRAME_PAYLOAD_MAX;
  uint8_t hdr_len = frame_rx_hdr_len(&_frame);
  uint16_t max_len = frame_rx_len(&_frame, _rx_max);
  radio_status_t st = hdr_len ? radio_rx_start_var(hdr_len, max_len, _rx_var_len, 0)
                              : radio_rx_start(max_len);
  if (st == RADIO_OK) _state = S_RX;
}

static bool _address_ok(uint8_t a)
{
  uint8_t mode = _reg[CC2500_07_PKTCTRL1] & 3;
  if (mode == 0 || a == _reg[CC2500_09_ADDR]) return true;
  return (mode >= 2 && a == 0x00) || (mode == 3 && a == 0xFF);
}

// Packets received since the last call into the RX FIFO
static void _rx_poll()
{
  if (_state == S_TX && radio_tx_done()) _state = S_IDLE;  // TXOFF_MODE IDLE
  while (_state == S_RX && radio_receive(&_pkt)) {
    _rx_busy = false;
    uint8_t payload[FRAME_PAYLOAD_MAX];
    uint8_t n = 0;
    frame_result_t r = frame_decode(&_frame, _pkt.data, _pkt.len, _rx_max, payload, &n);
    if (r == FRAME_SHORT || n == 0) continue;
    if (_frame.var_len && (n < 2 || !_address_ok(payload[1]))) continue;
    if (!_frame.var_len && !_address_ok(payload[0])) continue;
    bool crc_ok = r == FRAME_OK;
    if (!crc_ok && (_reg[CC2500_07_PKTCTRL1] & 0x08)) continue;  // CRC_AUTOFLUSH
    bool status = _reg[CC2500_07_PKTCTRL1] & 0x04;               // APPEND_STATUS
    if (_rx_len + n + (status ? 2 : 0) > FIFO_SIZE) continue;    // overflow
    memcpy(_rx_fifo + _rx_len, payload, n);
    _rx_len += n;
    _lqi = crc_ok ? 0x80 : 0;
    if (status) {
      int16_t rssi = (_pkt.rssi + RSSI_OFFSET) * 2;
      _rx_fifo[_rx_len++] = rssi < -128 ? 0x80 : rssi > 127 ? 0x7F : (uint8_t)rssi;
      _rx_fifo[_rx_len++] = _lqi;
    }
    // RXOFF_MODE: IDLE after a packet, else stays in RX
    if (!(_reg[CC2500_17_MCSM1] & 0x0C)) {
      radio_rx_stop();
      _state = S_IDLE;
    }
  }
}

void CC2500_WriteReg(uint8_t address, uint8_t data)
{
  address &= 0x3F;
  if (address < NREGS) _reg[address] = data;
  else if (address == CC2500_3E_PATABLE) {
    _patable = data;
    uint16_t power = _power_word(data);
    if (power != _power) {
      _power = _phy.tx_power = power;
      if (_phy_on) radio_set_tx_power(power);
    }
  } else if (address == CC2500_3F_TXFIFO && _tx_len < FIFO_SIZE) _tx_fifo[_tx_len++] = data;
}

void CC2500_WriteRegisterMulti(uint8_t address, const uint8_t data[], uint8_t length)
{
  address &= 0x3F;
  for (uint8_t i = 0; i < length; i++)
    CC2500_WriteReg(address < NREGS ? address + i : address, data[i]);
}

static uint8_t _status(uint8_t address)
{
  _rx_poll();
  switch (address) {
    case CC2500_30_PARTNUM: return 0x80;
    case CC2500_31_VERSION: return 0x03;
    case CC2500_33_LQI: return _lqi;
    case CC2500_34_RSSI: {
      int8_t dbm = radio_rssi();
      if (dbm == RADIO_RSSI_NONE) return 0x80;
      int16_t rssi = (dbm + RSSI_OFFSET) * 2;
      return rssi < -128 ? 0x80 : rssi > 127 ? 0x7F : (uint8_t)rssi;
    }
    case CC2500_35_MARCSTATE: return _state == S_RX ? MARC_RX : _state == S_TX ? MARC_TX : MARC_IDLE;
    case CC2500_3A_TXBYTES: return _tx_len;
    case CC2500_3B_RXBYTES:
      if (!_rx_len && _rx_busy && (int32_t)(_rx_end_us - micros()) < RX_WAIT_US) {
        while (!_rx_len && _rx_busy && (int32_t)(micros() - _rx_end_us) < 200) _rx_poll();
      }
      return _rx_len;
    default: return 0;
  }
}

uint8_t CC2500_ReadReg(uint8_t address)
{
  // Burst bit on 0x30..0x3D: status registers, else the strobe address
  if ((address & 0x3F) >= 0x30 && (address & CC2500_READ_BURST) == CC2500_READ_BURST)
    return _status(address & 0x3F);
  address &= 0x3F;
  if (address < NREGS) return _reg[address];
  if (address == CC2500_3E_PATABLE) return _patable;
  if (address == CC2500_3F_RXFIFO) {
    uint8_t b;
    CC2500_ReadData(&b, 1);
    return b;
  }
  return _status(address);
}

void CC2500_ReadRegisterMulti(uint8_t address, uint8_t data[], uint8_t length)
{
  address &= 0x3F;
  if (address == CC2500_3F_RXFIFO) {
    CC2500_ReadData(data, length);
    return;
  }
  for (uint8_t i = 0; i < length; i++) data[i] = CC2500_ReadReg(address + i);
}

void CC2500_ReadData(uint8_t* dpbuffer, uint8_t len)
{
  _rx_poll();
  uint8_t n = len < _rx_len ? len : _rx_len;
  memcpy(dpbuffer, _rx_fifo, n);
  memset(dpbuffer + n, 0, len - n);
  _rx_len -= n;
  memmove(_rx_fifo, _rx_fifo + n, _rx_len);
}

void CC2500_Strobe(uint8_t state)
{
  switch (state) {
    case CC2500_SRES:
      CC2500_Reset();
      break;
    case CC2500_STX:
      _tx();
      break;
    case CC2500_SRX:
      _rx();
      break;
    case CC2500_SIDLE:
    case CC2500_SPWD:
    case CC2500_SXOFF:
      if (_state == S_RX) radio_rx_stop();
      _rx_busy = false;
      _state = S_IDLE;
      break;
    case CC2500_SFRX:
      _rx_len = 0;
      break;
    case CC2500_SFTX:
      _tx_len = 0;
      break;
    default:  // SCAL, SFSTXON, SAFC, SWOR, SWORRST, SNOP
      break;
  }
}

void CC2500_WriteData(uint8_t* dpbuffer, uint8_t len)
{
  CC2500_Strobe(CC2500_SFTX);
  CC2500_WriteRegisterMulti(CC2500_3F_TXFIFO, dpbuffer, len);
  CC2500_Strobe(CC2500_STX);
}

// The GDO pins switch the 4-in-1's PA and LNA: the RF core does it here
void CC2500_SetTxRxMode(uint8_t mode) { (void)mode; }

uint8_t CC2500_Reset()
{
  if (_state == S_RX) radio_rx_stop();
  _rx_busy = false;
  _state = S_IDLE;
  memcpy(_reg, _reset_regs, NREGS);
  _patable = 0xC6;
  _power = RF_TX_POWER_LOW;
  _tx_len = _rx_len = 0;
  _lqi = 0;
  _phy_on = false;  // radio_init() at the next TX or RX
  return 1;
}

//
// CC2500_SPI.ino, unchanged
//

void CC2500_SetPower()
{
	uint8_t power=CC2500_BIND_POWER;
	if(IS_BIND_DONE)
		#ifdef CC2500_ENABLE_LOW_POWER
			power=IS_POWER_FLAG_on?CC2500_HIGH_POWER:CC2500_LOW_POWER;
		#else
			power=CC2500_HIGH_POWER;
		#endif
	if(IS_LBT_POWER_on)
	{
		power=CC2500_LBT_POWER;
		LBT_POWER_off;			// Only accept once
	}
	if(IS_RANGE_FLAG_on)
		power=CC2500_RANGE_POWER;
	if(prev_power != power)
	{
		CC2500_WriteReg(CC2500_3E_PATABLE, power);
		prev_power=power;
	}
}

void CC2500_SetFreqOffset()
{
	if(prev_option != option)
	{
		prev_option = option;
		CC2500_WriteReg(CC2500_0C_FSCTRL0, option);
	}
}
