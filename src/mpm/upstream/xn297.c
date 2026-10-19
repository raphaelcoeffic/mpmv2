// XN297_EMU.ino of the MULTI-Module on the CC2652: the MPM emulates the
// XN297 on an NRF24L01 (or a CC2500 at 250 kbps), here the RF core sends and
// receives the XN297 frames that frame.c builds. As the NRF24L01: TX starts
// with XN297_WritePayload() in TX mode, RX runs from XN297_SetTxRxMode(RX_EN)
// until the next mode change, a packet is kept until read
#include "iface_xn297.h"

#include "board.h"
#include "frame.h"
#include "radio.h"

#define NRF_FREQ_BASE_KHZ 2400000
#define SYNC_THRESHOLD_STRICT 0x32  // as the bench: repeated address patterns

// TX power words for the NRF power levels: high = the board default, the
// bind and range check levels (-18 dBm on the NRF24L01) 25 dB lower
#define POWER_HIGH RF_TX_POWER_DEFAULT
#define POWER_LOW RF_TX_POWER_LOW

static radio_phy_t _phy;
static bool _crc, _scramble, _250k;
static uint8_t _addr_len = 5;
static frame_t _tx, _tx_enh; // TX address: standard and enhanced framing
static frame_t _rx, _rx_enh; // RX address: standard and enhanced framing
static uint8_t _rx_len;      // payload length of XN297_SetRXAddr()
static enum TXRX_State _mode;
static uint8_t _channel;
static int16_t _freq_offset_khz;
static uint16_t _power;

static uint8_t _air[FRAME_AIR_MAX];  // packet on air, kept for XN297_ReSendPayload()
static uint16_t _air_len;
static radio_packet_t _pkt;
static bool _pkt_ready;

static void _frame_setup(frame_t* f, const uint8_t* addr, bool enhanced)
{
  memset(f, 0, sizeof(*f));
  f->type = FRAME_XN297;
  f->crc_len = _crc ? 2 : 0;
  f->scramble = _scramble;
  f->enhanced = enhanced;
  memcpy(f->addr, addr, _addr_len);
  f->addr_len = _addr_len;
  frame_init(f);
}

static void _set_sync()
{
  radio_set_sync_word(frame_tx_sync(&_tx), frame_rx_sync(&_rx));
}

static void _tune()
{
  radio_set_frequency(NRF_FREQ_BASE_KHZ + _channel * 1000 + _freq_offset_khz);
}

bool xn297_configure(bool crc_en, bool scramble_en, bool bitrate, bool force_nrf)
{
  _crc = crc_en;
  _scramble = scramble_en;
  _250k = bitrate == XN297_250K;
  _addr_len = 5;
  _mode = TXRX_OFF;
  _pkt_ready = false;
  _air_len = 0;
  _freq_offset_khz = 0;
  _power = POWER_LOW;  // bind power until XN297_SetPower()

  static const uint8_t zero[5] = {0};
  _frame_setup(&_tx, zero, false);
  _frame_setup(&_tx_enh, zero, true);
  _frame_setup(&_rx, zero, false);
  _frame_setup(&_rx_enh, zero, true);

  // The MPM uses its CC2500 at 250 kbps when it has one (RF tune option): its
  // channels and deviation (bench presets xn250k, nrf1m)
  if (_250k && !force_nrf) option_override = MPM_OPTION_RFTUNE;
  _phy = (radio_phy_t){
      .symbol_rate = _250k ? 250000 : 1000000,
      .deviation = _250k ? 127000 : 160000,
      .rx_bw = _250k ? 94 : 97,
      .preamble_bytes = 4,
      .sync_bits = frame_sync_bits(&_tx),
      .if_freq = (int16_t)0x8000,
      .tx_power = _power,
  };
  _set_sync();
  if (radio_init(&_phy) != RADIO_OK) {
    SUB_PROTO_INVALID;
    return false;
  }
  return true;
}

void XN297_SetTXAddr(const uint8_t* addr, uint8_t len)
{
  if (len > 5) len = 5;
  if (len < 3) len = 3;
  _addr_len = len;
  uint8_t pid = _tx_enh.pid;
  _frame_setup(&_tx, addr, false);
  _frame_setup(&_tx_enh, addr, true);
  _tx_enh.pid = pid;
  _set_sync();
}

void XN297_SetRXAddr(const uint8_t* addr, uint8_t rx_packet_len)
{
  _frame_setup(&_rx, addr, false);
  _frame_setup(&_rx_enh, addr, true);
  _rx_len = rx_packet_len;
  _set_sync();

  // Addresses with repeated patterns: stricter sync word correlation
  uint8_t threshold = frame_rx_sync_weak(&_rx) ? SYNC_THRESHOLD_STRICT : 0;
  if (threshold != _phy.sync_threshold) {
    _phy.sync_threshold = threshold;
    _phy.tx_power = _power;
    radio_init(&_phy);
  }
}

void XN297_SetTxRxMode(enum TXRX_State mode)
{
  _mode = mode;
  _pkt_ready = false;
  if (mode != RX_EN) {
    radio_rx_stop();
    return;
  }
  // As the MPM on the NRF24L01: payload, 2 bytes more for an enhanced packet
  // control field, CRC
  radio_rx_start(frame_rx_len(&_rx, _rx_len) + 2);
}

static void _send(uint16_t n)
{
  _air_len = n;
  if (n) radio_tx_start(_air, n);
}

void XN297_WritePayload(uint8_t* msg, uint8_t len)
{
  _send(frame_tx_data(&_tx, msg, len, _air));
}

void XN297_WriteEnhancedPayload(uint8_t* msg, uint8_t len, uint8_t noack)
{
  _tx_enh.noack = noack;
  _send(frame_tx_data(&_tx_enh, msg, len, _air));
}

void XN297_ReSendPayload()
{
  _send(_air_len);
}

bool XN297_IsRX()
{
  if (!_pkt_ready && _mode == RX_EN) _pkt_ready = radio_receive(&_pkt);
  return _pkt_ready;
}

bool XN297_ReadPayload(uint8_t* msg, uint8_t len)
{
  uint8_t payload[FRAME_PAYLOAD_MAX];
  uint8_t n = 0;
  _pkt_ready = false;
  frame_result_t r = frame_decode(&_rx, _pkt.data, _pkt.len, len, payload, &n);
  memcpy(msg, payload, n < len ? n : len);
  return r == FRAME_OK;
}

uint8_t XN297_ReadEnhancedPayload(uint8_t* msg, uint8_t len)
{
  uint8_t payload[FRAME_PAYLOAD_MAX];
  uint8_t n = 0;
  uint16_t pcf;
  _pkt_ready = false;
  if (frame_decode_pcf(&_rx_enh, _pkt.data, _pkt.len, len, payload, &n, &pcf) != FRAME_OK)
    return 255;
  memcpy(msg, payload, n);
  return n;
}

bool XN297_IsPacketSent() { return radio_tx_done(); }

void XN297_HoppingCalib(uint8_t num_freq) { (void)num_freq; }

void XN297_Hopping(uint8_t index) { XN297_RFChannel(hopping_frequency[index]); }

void XN297_RFChannel(uint8_t number)
{
  _channel = number;
  _tune();
  // The NRF24L01 keeps receiving on the new channel
  if (_mode == RX_EN) XN297_SetTxRxMode(RX_EN);
}

void XN297_SetPower()
{
  uint16_t power = IS_BIND_DONE ? POWER_HIGH : POWER_LOW;
  if (IS_RANGE_FLAG_on) power = POWER_LOW;
  if (power == _power) return;
  _power = power;
  radio_set_tx_power(power);
}

// CC2500 FSCTRL0 = option: 26 MHz / 2^14 = 1.587 kHz a step, from the next
// channel change
void XN297_SetFreqOffset()
{
  if (_250k) _freq_offset_khz = (int8_t)option * 1587 / 1000;
}
