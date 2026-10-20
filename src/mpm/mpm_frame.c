#include "mpm_frame.h"

#include <string.h>

// Header: 0x55 / 0x54 = channels with protocol 0..31 / 32..63, 0x57 / 0x56:
// failsafe values instead
#define HDR_MASK 0xFC
#define HDR 0x54
#define HDR_LOW_PROTO 0x01
#define HDR_FAILSAFE 0x02

// Byte 1: protocol bits 0-4, then these
#define B1_BIND 0x80
#define B1_AUTOBIND 0x40
#define B1_RANGE_CHECK 0x20
// Byte 2: rx_num bits 0-3, sub_protocol bits 4-6, then
#define B2_LOW_POWER 0x80
// Byte 26 (V2): protocol bits 6-7, rx_num bits 4-5, then
#define B26_TELEM_INVERT MPM_FRAME_TELEM_INVERT
#define B26_DISABLE_TELEM 0x02
#define B26_DISABLE_CH_MAP 0x01

// Channels: 16 x 11 bits from byte 4, least significant bit first (as SBUS)
#define CHANNELS_POS 4

bool mpm_frame_decode(const uint8_t* buf, uint8_t len, mpm_frame_t* f)
{
  if (len < MPM_FRAME_V1_LEN || len > MPM_FRAME_MAX_LEN || (buf[0] & HDR_MASK) != HDR)
    return false;

  memset(f, 0, sizeof(*f));
  f->failsafe = buf[0] & HDR_FAILSAFE;
  f->protocol = (buf[1] & 0x1F) + (buf[0] & HDR_LOW_PROTO ? 0 : 32);
  f->bind = buf[1] & B1_BIND;
  f->autobind = buf[1] & B1_AUTOBIND;
  f->range_check = buf[1] & B1_RANGE_CHECK;
  f->rx_num = buf[2] & 0x0F;
  f->sub_protocol = (buf[2] >> 4) & 0x07;
  f->low_power = buf[2] & B2_LOW_POWER;
  f->option = (int8_t)buf[3];

  for (uint8_t i = 0; i < MPM_CHANNELS; i++) {
    uint16_t bit = 11 * i;
    const uint8_t* p = buf + CHANNELS_POS + bit / 8;
    uint32_t v = p[0] | p[1] << 8 | (uint32_t)p[2] << 16;
    f->channels[i] = (v >> (bit % 8)) & 0x7FF;
  }

  if (len > MPM_FRAME_V1_LEN) {
    uint8_t b26 = buf[MPM_FRAME_V1_LEN];
    f->v2 = true;
    f->protocol |= b26 & 0xC0;
    f->rx_num |= b26 & 0x30;
    f->telem_invert = b26 & B26_TELEM_INVERT;
    f->disable_telem = b26 & B26_DISABLE_TELEM;
    f->disable_ch_map = b26 & B26_DISABLE_CH_MAP;
    f->data_len = len - MPM_FRAME_V1_LEN - 1;
    memcpy(f->data, buf + MPM_FRAME_V1_LEN + 1, f->data_len);
  }
  return true;
}

uint8_t mpm_frame_encode(const mpm_frame_t* f, uint8_t* buf)
{
  memset(buf, 0, MPM_FRAME_MAX_LEN);
  buf[0] = HDR | (f->failsafe ? HDR_FAILSAFE : 0) | ((f->protocol & 0x20) ? 0 : HDR_LOW_PROTO);
  buf[1] = (f->protocol & 0x1F) | (f->bind ? B1_BIND : 0) | (f->autobind ? B1_AUTOBIND : 0) |
           (f->range_check ? B1_RANGE_CHECK : 0);
  buf[2] = (f->rx_num & 0x0F) | (f->sub_protocol & 0x07) << 4 | (f->low_power ? B2_LOW_POWER : 0);
  buf[3] = (uint8_t)f->option;

  for (uint8_t i = 0; i < MPM_CHANNELS; i++) {
    uint16_t bit = 11 * i;
    uint8_t* p = buf + CHANNELS_POS + bit / 8;
    uint32_t v = (uint32_t)(f->channels[i] & 0x7FF) << (bit % 8);
    p[0] |= v;
    p[1] |= v >> 8;
    p[2] |= v >> 16;
  }

  if (!f->v2) return MPM_FRAME_V1_LEN;
  buf[MPM_FRAME_V1_LEN] = (f->protocol & 0xC0) | (f->rx_num & 0x30) |
                          (f->telem_invert ? B26_TELEM_INVERT : 0) |
                          (f->disable_telem ? B26_DISABLE_TELEM : 0) |
                          (f->disable_ch_map ? B26_DISABLE_CH_MAP : 0);
  uint8_t n = f->data_len > MPM_FRAME_DATA_MAX ? MPM_FRAME_DATA_MAX : f->data_len;
  memcpy(buf + MPM_FRAME_V1_LEN + 1, f->data, n);
  return MPM_FRAME_V1_LEN + 1 + n;
}

uint8_t mpm_telem_header(uint8_t* buf, uint8_t type, uint8_t len)
{
  buf[0] = 'M';
  buf[1] = 'P';
  buf[2] = type;
  buf[3] = len;
  return MPM_TELEM_HDR_LEN;
}
