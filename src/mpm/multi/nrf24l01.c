// NRF24l01_SPI.ino of the MULTI-Module on the CC2652: an nRF24L01+ behind
// its registers. The protocols write the registers and the TX payload as on
// the chip; the address width, addresses, CRC, ShockBurst / Enhanced
// ShockBurst (EN_AA, DYNPD, FEATURE) and data rate registers become the RF
// core setup and frame.c's NRF framing. In TX mode (CONFIG PWR_UP, PRIM_RX
// clear, NRF24L01_SetTxRxMode(TX_EN)) a payload goes on air when written, as
// with CE high; TX_DS is set once it is sent. In RX mode, pipe 0 receives
// (RX_ADDR_P0, RX_PW_P0 or the dynamic length) into a 3 payload RX FIFO,
// packets with a bad CRC dropped as on the chip; RX_DR set, RPD from the RSSI.
//
// Not emulated: auto acknowledgement and retransmissions (with EN_AA the
// packet control field is sent and TX_DS set once the packet is out: no ack
// is received, MAX_RT never set), ack payloads, RX pipes 1 to 5, the 3
// payload TX FIFO (one payload, sent at once)
#include "chips.h"

#include "board.h"
#include "frame.h"
#include "radio.h"

#define NRF_FREQ_BASE_KHZ 2400000
#define NREGS 0x1E
#define RX_FIFO_DEPTH 3
#define RPD_DBM (-64)  // RPD: received power above -64 dBm

// CONFIG
#define CONFIG_PRIM_RX 0x01
#define CONFIG_PWR_UP 0x02
#define CONFIG_CRCO 0x04
#define CONFIG_EN_CRC 0x08
// STATUS
#define STATUS_RX_DR 0x40
#define STATUS_TX_DS 0x20
#define STATUS_MAX_RT 0x10
#define STATUS_RX_P_NO_EMPTY 0x0E
// FIFO_STATUS
#define FIFO_RX_EMPTY 0x01
#define FIFO_RX_FULL 0x02
#define FIFO_TX_EMPTY 0x10
// RF_SETUP
#define RF_DR_LOW 0x20
#define RF_DR_HIGH 0x08
// FEATURE
#define FEATURE_EN_DPL 0x04

// Reset values (datasheet section 9), 0x00 to 0x1D. The 5 byte addresses
// are kept apart
static const uint8_t _reset_regs[NREGS] = {
    0x08, 0x3F, 0x03, 0x03, 0x03, 0x02, 0x0E, 0x0E, 0x00, 0x00, 0x00, 0x00, 0xC3, 0xC4, 0xC5,
    0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static uint8_t _reg[NREGS];
static uint8_t _rx_addr_p0[5], _rx_addr_p1[5], _tx_addr[5];
static uint8_t _status;
static bool _ce;  // NRF24L01_SetTxRxMode(): CE high in TX and RX
static enum { S_OFF, S_RX, S_TX } _state;

static uint8_t _tx_payload[FRAME_NRF_PAYLOAD_MAX], _tx_len;
static bool _tx_busy;  // sent, TX_DS not yet set
static uint8_t _rx_fifo[RX_FIFO_DEPTH][FRAME_NRF_PAYLOAD_MAX], _rx_fifo_len[RX_FIFO_DEPTH];
static uint8_t _rx_count;

static frame_t _tx, _rx;
static radio_phy_t _phy;
static bool _phy_on, _setup_dirty;
static uint16_t _power;
static radio_packet_t _pkt;
static uint8_t _air[FRAME_AIR_MAX];  // packet on air, kept for REUSE_TX_PL
static uint16_t _air_len;

uint8_t rf_setup;

// TX power word for RF_SETUP RF_PWR (-18, -12, -6, 0 dBm): dB below full
// power on a 4-in-1, whose PA compresses the top steps. The MPM's bind and
// range level (-18 dBm) 17 dB down, its low power (-12 dBm) 9
static uint16_t _power_word()
{
  static const uint8_t db[] = {17, 9, 4, 0};
  return rf_power_word(db[(_reg[NRF24L01_06_RF_SETUP] >> 1) & 3]);
}

static uint8_t _addr_len()
{
  uint8_t aw = _reg[NRF24L01_03_SETUP_AW] & 3;
  return aw ? aw + 2 : 5;  // 0 is illegal
}

static bool _dpl() { return (_reg[NRF24L01_1D_FEATURE] & FEATURE_EN_DPL) && (_reg[NRF24L01_1C_DYNPD] & 1); }

static bool _frame_setup(frame_t* f, const uint8_t* addr)
{
  uint8_t config = _reg[NRF24L01_00_CONFIG];
  memset(f, 0, sizeof(*f));
  f->type = FRAME_NRF;
  memcpy(f->addr, addr, FRAME_ADDR_MAX);
  f->addr_len = _addr_len();
  f->crc_len = config & CONFIG_EN_CRC ? (config & CONFIG_CRCO ? 2 : 1) : 0;
  // The packet control field: auto acknowledgement or dynamic payload length
  // on pipe 0
  f->dpl = _dpl();
  f->esb = (_reg[NRF24L01_01_EN_AA] & 1) || f->dpl;
  f->sync_bytes = 4;
  return frame_init(f);
}

// PHY and framings from the registers (frame_init(): about 170 us, only
// after a register change): false if not supported
static bool _prepare()
{
  if (_setup_dirty || !_phy_on) {
    frame_t tx, rx;
    if (!_frame_setup(&tx, _tx_addr) || !_frame_setup(&rx, _rx_addr_p0)) return false;
    uint8_t pid = _tx.pid;
    _tx = tx;
    _tx.pid = pid;
    _rx = rx;

    // The bench presets nrf250k, nrf1m and nrf2m
    uint8_t dr = _reg[NRF24L01_06_RF_SETUP];
    radio_phy_t phy = {
        .symbol_rate = dr & RF_DR_LOW ? 250000 : dr & RF_DR_HIGH ? 2000000 : 1000000,
        .deviation = dr & RF_DR_LOW ? 160000 : dr & RF_DR_HIGH ? 320000 : 160000,
        .rx_bw = dr & RF_DR_LOW ? 94 : dr & RF_DR_HIGH ? 100 : 97,
        .preamble_bytes = 4,
        .sync_bits = frame_sync_bits(&_tx),
        .if_freq = (int16_t)0x8000,
        .tx_power = _power,
        .sync_threshold = frame_rx_sync_weak(&_rx) ? 0x32 : 0,
    };
    if (!_phy_on || memcmp(&phy, &_phy, sizeof(phy))) {
      _phy = phy;
      if (radio_init(&_phy) != RADIO_OK) {
        _phy_on = false;
        return false;
      }
      _phy_on = true;
      radio_set_frequency(NRF_FREQ_BASE_KHZ + _reg[NRF24L01_05_RF_CH] * 1000);
    }
    radio_set_sync_word(frame_tx_sync(&_tx), frame_rx_sync(&_rx));
    _setup_dirty = false;
  }
  return true;
}

static void _send()
{
  if (!_tx_len || !_prepare()) return;
  _air_len = frame_tx_data(&_tx, _tx_payload, _tx_len, _air);
  if (!_air_len) return;
  radio_tx_start(_air, _air_len);
  _tx_busy = true;
  _tx_len = 0;  // the FIFO entry leaves when sent
}

static void _rx_start()
{
  if (!_prepare()) return;
  uint8_t len = _dpl() ? FRAME_NRF_PAYLOAD_MAX : _reg[NRF24L01_11_RX_PW_P0] & 0x3F;
  if (len > FRAME_NRF_PAYLOAD_MAX) len = FRAME_NRF_PAYLOAD_MAX;
  uint16_t max_len = frame_rx_len(&_rx, len);
  if (!max_len) return;
  if (radio_rx_start(max_len) == RADIO_OK) _state = S_RX;
}

// The mode CONFIG and CE give: standby (TX) sends what comes, RX restarts
static void _update_mode()
{
  uint8_t config = _reg[NRF24L01_00_CONFIG];
  if (_state == S_RX) radio_rx_stop();
  _state = S_OFF;
  if (!_ce || !(config & CONFIG_PWR_UP)) return;
  if (config & CONFIG_PRIM_RX) {
    _rx_start();
  } else {
    _state = S_TX;
    _send();
  }
}

// TX done and packets received since the last call
static void _poll()
{
  if (_tx_busy && radio_tx_done()) {
    _tx_busy = false;
    _status |= STATUS_TX_DS;
  }
  while (_state == S_RX && radio_receive(&_pkt)) {
    uint8_t payload[FRAME_PAYLOAD_MAX];
    uint8_t n = 0;
    uint8_t len = _dpl() ? FRAME_NRF_PAYLOAD_MAX : _reg[NRF24L01_11_RX_PW_P0] & 0x3F;
    if (frame_decode(&_rx, _pkt.data, _pkt.len, len, payload, &n) != FRAME_OK) continue;
    if (_rx_count == RX_FIFO_DEPTH) continue;  // full: the chip drops it
    if (n > FRAME_NRF_PAYLOAD_MAX) n = FRAME_NRF_PAYLOAD_MAX;
    memcpy(_rx_fifo[_rx_count], payload, n);
    _rx_fifo_len[_rx_count++] = n;
    _status |= STATUS_RX_DR;
  }
}

void NRF24L01_WriteReg(uint8_t reg, uint8_t data)
{
  reg &= REGISTER_MASK;
  switch (reg) {
    case NRF24L01_07_STATUS:
      _status &= ~(data & (STATUS_RX_DR | STATUS_TX_DS | STATUS_MAX_RT));
      return;
    case NRF24L01_0A_RX_ADDR_P0:  // one byte written: the first (LSB)
      _rx_addr_p0[0] = data;
      _setup_dirty = true;
      return;
    case NRF24L01_0B_RX_ADDR_P1:
      _rx_addr_p1[0] = data;
      return;
    case NRF24L01_10_TX_ADDR:
      _tx_addr[0] = data;
      _setup_dirty = true;
      return;
    case NRF24L01_17_FIFO_STATUS:
    case NRF24L01_08_OBSERVE_TX:
    case NRF24L01_09_CD:
      return;  // read only
  }
  if (reg >= NREGS) return;
  uint8_t old = _reg[reg];
  _reg[reg] = data;
  switch (reg) {
    case NRF24L01_00_CONFIG:
      if ((old ^ data) & (CONFIG_EN_CRC | CONFIG_CRCO)) _setup_dirty = true;
      if ((old ^ data) & (CONFIG_PWR_UP | CONFIG_PRIM_RX | CONFIG_EN_CRC | CONFIG_CRCO))
        _update_mode();
      break;
    case NRF24L01_05_RF_CH:
      if (_phy_on) radio_set_frequency(NRF_FREQ_BASE_KHZ + (data & 0x7F) * 1000);
      if (_state == S_RX) _update_mode();  // keeps receiving on the new channel
      break;
    case NRF24L01_06_RF_SETUP: {
      uint16_t power = _power_word();
      if (power != _power) {
        _power = _phy.tx_power = power;
        if (_phy_on) radio_set_tx_power(power);
      }
      if ((old ^ data) & (RF_DR_LOW | RF_DR_HIGH)) _setup_dirty = true;
      break;
    }
    case NRF24L01_01_EN_AA:
    case NRF24L01_03_SETUP_AW:
    case NRF24L01_11_RX_PW_P0:
    case NRF24L01_1C_DYNPD:
    case NRF24L01_1D_FEATURE:
      if (old != data) _setup_dirty = true;
      break;
  }
}

void NRF24L01_WriteRegisterMulti(uint8_t reg, uint8_t* data, uint8_t length)
{
  reg &= REGISTER_MASK;
  uint8_t* addr = reg == NRF24L01_0A_RX_ADDR_P0 ? _rx_addr_p0
                  : reg == NRF24L01_0B_RX_ADDR_P1 ? _rx_addr_p1
                  : reg == NRF24L01_10_TX_ADDR   ? _tx_addr
                                                 : 0;
  if (!addr) {
    if (length) NRF24L01_WriteReg(reg, data[0]);  // other registers: 1 byte
    return;
  }
  memcpy(addr, data, length < 5 ? length : 5);
  if (addr != _rx_addr_p1) _setup_dirty = true;
}

void NRF24L01_WritePayload(uint8_t* data, uint8_t length)
{
  if (length > FRAME_NRF_PAYLOAD_MAX) length = FRAME_NRF_PAYLOAD_MAX;
  memcpy(_tx_payload, data, length);
  _tx_len = length;
  if (_state == S_TX) _send();
}

uint8_t NRF24L01_ReadReg(uint8_t reg)
{
  reg &= REGISTER_MASK;
  _poll();
  switch (reg) {
    case NRF24L01_07_STATUS:
      return _status | (_rx_count ? 0 : STATUS_RX_P_NO_EMPTY);
    case NRF24L01_08_OBSERVE_TX:
      return 0;
    case NRF24L01_09_CD: {
      int8_t dbm = radio_rssi();
      return _state == S_RX && dbm != RADIO_RSSI_NONE && dbm >= RPD_DBM;
    }
    case NRF24L01_0A_RX_ADDR_P0:
      return _rx_addr_p0[0];
    case NRF24L01_0B_RX_ADDR_P1:
      return _rx_addr_p1[0];
    case NRF24L01_10_TX_ADDR:
      return _tx_addr[0];
    case NRF24L01_17_FIFO_STATUS:
      return (_rx_count ? 0 : FIFO_RX_EMPTY) | (_rx_count == RX_FIFO_DEPTH ? FIFO_RX_FULL : 0) |
             (_tx_len || _tx_busy ? 0 : FIFO_TX_EMPTY);
  }
  return reg < NREGS ? _reg[reg] : 0;
}

void NRF24L01_ReadPayload(uint8_t* data, uint8_t length)
{
  _poll();
  uint8_t n = _rx_count ? _rx_fifo_len[0] : 0;
  if (n > length) n = length;
  memcpy(data, _rx_fifo[0], n);
  memset(data + n, 0, length - n);
  if (!_rx_count) return;
  _rx_count--;
  memmove(_rx_fifo[0], _rx_fifo[1], sizeof(_rx_fifo[0]) * _rx_count);
  memmove(_rx_fifo_len, _rx_fifo_len + 1, _rx_count);
}

uint8_t NRF24L01_GetDynamicPayloadSize()
{
  _poll();
  return _rx_count ? _rx_fifo_len[0] : 0;
}

void NRF24L01_Strobe(uint8_t state)
{
  switch (state) {
    case FLUSH_TX:
      _tx_len = 0;
      break;
    case FLUSH_RX:
      _rx_count = 0;
      break;
    case REUSE_TX_PL:  // the last packet sent, again (same packet id)
      if (_state == S_TX && _air_len) {
        radio_tx_start(_air, _air_len);
        _tx_busy = true;
      }
      break;
    default:  // NOP
      break;
  }
}

void NRF24L01_FlushTx() { NRF24L01_Strobe(FLUSH_TX); }

void NRF24L01_FlushRx() { NRF24L01_Strobe(FLUSH_RX); }

// CE: the RF core starts TX and RX itself, no settling delay
void NRF24L01_SetTxRxMode(enum TXRX_State mode)
{
  NRF24L01_WriteReg(NRF24L01_07_STATUS, STATUS_RX_DR | STATUS_TX_DS | STATUS_MAX_RT);
  _ce = mode != TXRX_OFF;
  uint8_t config = 1 << NRF24L01_00_EN_CRC;
  if (mode != TXRX_OFF) config |= 1 << NRF24L01_00_CRCO | 1 << NRF24L01_00_PWR_UP;
  if (mode == RX_EN) config |= 1 << NRF24L01_00_PRIM_RX;
  if ((_reg[NRF24L01_00_CONFIG] ^ config) & (CONFIG_EN_CRC | CONFIG_CRCO)) _setup_dirty = true;
  _reg[NRF24L01_00_CONFIG] = config;
  _update_mode();  // CE low then high: RX restarts
}

void NRF24L01_Reset()
{
  if (_state == S_RX) radio_rx_stop();
  _state = S_OFF;
  _ce = false;
  memcpy(_reg, _reset_regs, NREGS);
  static const uint8_t p0[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
  static const uint8_t p1[5] = {0xC2, 0xC2, 0xC2, 0xC2, 0xC2};
  memcpy(_rx_addr_p0, p0, 5);
  memcpy(_tx_addr, p0, 5);
  memcpy(_rx_addr_p1, p1, 5);
  _status = 0x0E & ~STATUS_RX_P_NO_EMPTY;
  _tx_len = _rx_count = 0;
  _air_len = 0;
  _tx_busy = false;
  _power = _power_word();
  _phy_on = false;  // radio_init() at the next TX or RX
  _setup_dirty = true;
}

//
// NRF24l01_SPI.ino, unchanged but for the reset first (the chip keeps its
// registers from the previous protocol)
//

void NRF24L01_Initialize()
{
    NRF24L01_Reset();
    rf_setup = 0x09;
	prev_power = 0x00;	// Make sure prev_power is inline with current power

	//Load most likely default NRF config
	NRF24L01_FlushTx();
	NRF24L01_FlushRx();
	NRF24L01_WriteReg(NRF24L01_01_EN_AA,		0x00);	// No Auto Acknowldgement on all data pipes
	NRF24L01_WriteReg(NRF24L01_02_EN_RXADDR,	0x01); 	// Enable data pipe 0 only
	NRF24L01_WriteReg(NRF24L01_03_SETUP_AW,		0x03);	// 5 bytes rx/tx address
	NRF24L01_WriteReg(NRF24L01_04_SETUP_RETR,	0x00); 	// no retransmits
	NRF24L01_SetBitrate(NRF24L01_BR_1M);             	// 1Mbps
	NRF24L01_WriteReg(NRF24L01_1C_DYNPD, 0x00);       	// Disable dynamic payload length on all pipes
	NRF24L01_WriteReg(NRF24L01_1D_FEATURE, 0x01);     	// Set feature bits off and enable the command NRF24L01_B0_TX_PYLD_NOACK
	NRF24L01_SetPower();
	NRF24L01_SetTxRxMode(TX_EN);						// Clear data ready, data sent, retransmit and enable CRC 16bits, ready for TX
}

void NRF24L01_SetBitrate(uint8_t bitrate)
{
    // Note that bitrate 250kbps (and bit RF_DR_LOW) is valid only
    // for nRF24L01+. There is no way to programmatically tell it from
    // older version, nRF24L01, but the older is practically phased out
    // by Nordic, so we assume that we deal with modern version.

    // Bit 0 goes to RF_DR_HIGH, bit 1 - to RF_DR_LOW
    rf_setup = (rf_setup & 0xD7) | ((bitrate & 0x02) << 4) | ((bitrate & 0x01) << 3);
    prev_power=(rf_setup>>1)&0x03;	// Make sure prev_power is inline with current power
	NRF24L01_WriteReg(NRF24L01_06_RF_SETUP, rf_setup);
}

void NRF24L01_SetPower()
{
	uint8_t power=NRF_BIND_POWER;
	if(IS_BIND_DONE)
		#ifdef NRF24L01_ENABLE_LOW_POWER
			power=IS_POWER_FLAG_on?NRF_HIGH_POWER:NRF_LOW_POWER;
		#else
			power=NRF_HIGH_POWER;
		#endif
	if(IS_RANGE_FLAG_on)
		power=NRF_POWER_0;
	if(prev_power != power)
	{
		rf_setup = (rf_setup & 0xF8) | (power << 1);
		if(power==3)
			rf_setup |= 0x01;	// Si24r01 full power, unused bit for NRF
		NRF24L01_WriteReg(NRF24L01_06_RF_SETUP, rf_setup);
		prev_power=power;
	}
}

uint8_t NRF24L01_packet_ack()
{
    switch (NRF24L01_ReadReg(NRF24L01_07_STATUS) & (_BV(NRF24L01_07_TX_DS) | _BV(NRF24L01_07_MAX_RT)))
	{
		case _BV(NRF24L01_07_TX_DS):
			return PKT_ACKED;
		case _BV(NRF24L01_07_MAX_RT):
			return PKT_TIMEOUT;
    }
	return PKT_PENDING;
}
