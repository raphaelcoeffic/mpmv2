// Boot loader (flash from 0 to APP_START): the application update from the
// radio
//
// Speaks the STK500v1 subset EdgeTX uses to flash a MULTI-Module
// (radio/src/io/multi_firmware_update.cpp), at 57600 8N1, on the radio link
// and on the console UART (bl_port.h: the chip's part, platforms/<family>/
// bl_port.c; the board's pins in its board.h). See docs/update.md.
//
// At power up: no valid application -> serve STK500 until one is written.
// Otherwise listen for STK_GET_SYNC for up to BL_WINDOW_MS, and start the
// application as soon as the radio link shows normal traffic (serial frames
// or PPM at another rate: framing errors, other bytes) or no radio at all
// (bl_radio_absent()): a radio rebooting with the model in the air gets
// its module back with its first frames. The full window only runs out with
// the line idle and driven, as EdgeTX does when it flashes (it powers the
// module up and syncs at once for an MPM v2 file).
//
// The image CRC is checked once, at the first start after an update (it
// takes about 0.3 ms per KB on the CC2652): the header's `checked` word is
// then cleared.

#include <stdbool.h>
#include <string.h>

#include "app_image.h"
#include "bl_layout.h"
#include "bl_port.h"

#define BL_WINDOW_MS       300   // longest wait for a sync at power up
#define BL_CONSOLE_MS      20    // shortest wait (host tools reset, then sync)
#define BL_ABSENT_MS       20    // radio line at the "no radio" level
#define BL_NOISE_BYTES     8     // radio traffic that isn't STK500
#define BL_BYTE_TIMEOUT_MS 100   // inside a command
#define BL_IDLE_MS         10000 // session idle with a valid application

// STK500 device signature: 0x1E (required by EdgeTX), then "U&": the MPM v2
// boot loader, 256 byte pages from word 0x1000 (the STM32 MULTI boot loader
// answers 1E 55 AA). The image header gives the chip
#define BL_SIGNATURE_0 0x1E
#define BL_SIGNATURE_1 0x55
#define BL_SIGNATURE_2 0x26

#define STK_OK              0x10
#define STK_FAILED          0x11
#define STK_UNKNOWN         0x12
#define STK_INSYNC          0x14
#define STK_NOSYNC          0x15
#define CRC_EOP             0x20
#define STK_GET_SYNC        0x30
#define STK_GET_SIGN_ON     0x31
#define STK_GET_PARAMETER   0x41
#define STK_SET_DEVICE      0x42
#define STK_ENTER_PROGMODE  0x50
#define STK_LEAVE_PROGMODE  0x51
#define STK_LOAD_ADDRESS    0x55
#define STK_PROG_PAGE       0x64
#define STK_READ_PAGE       0x74
#define STK_READ_SIGN       0x75

#define PAGE_MAX 256

// Cortex-M (ARMv7-M, ARMv8-M): the vector table offset register
#define SCB_VTOR 0xE000ED08

static uint32_t _n_ports;
static bool _radio;     // a port is the radio's link
static uint32_t _port;  // the one that sent the sync

// --- CRC32 (zlib) ---

static uint32_t _crc_table[256];

static void crc_init()
{
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
    _crc_table[i] = c;
  }
}

static uint32_t crc_update(uint32_t crc, const uint8_t* p, uint32_t len)
{
  while (len--) crc = _crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}

// --- application ---

static bool vectors_ok(const uint32_t* vt)
{
  uint32_t sp = vt[0], pc = vt[1];
  return sp > SRAM_START && sp <= SRAM_END && !(sp & 3) && (pc & 1) &&
         (pc & ~1) >= APP_START + 0x40 && (pc & ~1) < APP_END;
}

// The header of an image for this chip in its first len bytes, or 0
static app_header_t* header_find(uint8_t* img, uint32_t len)
{
  uint32_t off = app_header_offset(img, len);
  if (!off) return 0;
  app_header_t* h = (app_header_t*)(img + off);
  if (h->chip != PLATFORM_CHIP_ID || h->length < off + sizeof(*h) ||
      h->length > APP_END - APP_START)
    return 0;
  return h;
}

static const app_header_t* app_header()
{
  return header_find((uint8_t*)APP_START, APP_HDR_SEARCH);
}

static bool app_checked()
{
  const app_header_t* h = app_header();
  return vectors_ok((const uint32_t*)APP_START) && h && h->checked == APP_HDR_CHECKED;
}

static bool app_valid()
{
  const uint8_t* img = (const uint8_t*)APP_START;
  const app_header_t* h = app_header();
  if (!vectors_ok((const uint32_t*)img) || !h) return false;

  static const uint8_t zeros[sizeof(app_header_t)];
  uint32_t off = (const uint8_t*)h - img;
  uint32_t crc = 0xFFFFFFFF;
  crc = crc_update(crc, img, off);
  crc = crc_update(crc, zeros, sizeof(zeros));
  crc = crc_update(crc, img + off + sizeof(zeros), h->length - off - sizeof(zeros));
  return ~crc == h->crc;
}

// Cortex-M: the application's vector table, its stack, its reset handler
static void app_start()
{
  bl_deinit();
  *(volatile uint32_t*)SCB_VTOR = APP_START;

  const uint32_t* vt = (const uint32_t*)APP_START;
  __asm volatile(
      "msr msp, %0\n"
      "bx %1\n" ::"r"(vt[0]),
      "r"(vt[1]));
  while (1) {}
}

// --- serial ---

static void put(uint8_t b) { bl_port_write(_port, b); }

static bool get(uint8_t* b)
{
  uint32_t start = bl_millis();
  while (bl_millis() - start < BL_BYTE_TIMEOUT_MS) {
    int v = bl_port_read(_port);
    if (v < 0 || (v & BL_RX_ERROR)) continue;
    *b = v;
    return true;
  }
  return false;
}

// Power up window: true when a port sent a sync (_port), false to start
// the application
static bool wait_sync(bool forever)
{
  uint8_t prev[BL_PORTS_MAX] = {0};
  uint32_t noise = 0;
  uint32_t start = bl_millis();
  uint32_t present = start;

  while (true) {
    for (uint32_t i = 0; i < _n_ports; i++) {
      int v;
      while ((v = bl_port_read(i)) >= 0) {
        uint8_t b = v;
        bool error = v & BL_RX_ERROR;
        if (!error && prev[i] == STK_GET_SYNC && b == CRC_EOP) {
          _port = i;
          return true;
        }
        if (bl_port_radio(i) && (error || b != STK_GET_SYNC)) noise++;
        prev[i] = error ? 0 : b;
      }
    }
    if (forever) continue;

    uint32_t t = bl_millis() - start;
    if (t < BL_CONSOLE_MS) continue;
    if (!_radio || noise > BL_NOISE_BYTES) return false;
    if (!bl_radio_absent())
      present = bl_millis();
    else if (bl_millis() - present >= BL_ABSENT_MS)
      return false;
    if (t >= BL_WINDOW_MS) return false;
  }
}

// --- flash ---

#define N_SECTORS (APP_END / FLASH_SECTOR_SIZE)

static bool _flash_open;
static uint32_t _erased[(N_SECTORS + 31) / 32];  // sectors erased in this session

static bool flash_write(uint32_t addr, uint8_t* data, uint32_t len)
{
  if (addr < APP_START || addr + len > APP_END) return false;

  bool ok = true;
  for (uint32_t s = addr / FLASH_SECTOR_SIZE;
       ok && s <= (addr + len - 1) / FLASH_SECTOR_SIZE; s++) {
    if (!(_erased[s / 32] & (1u << s % 32))) {
      ok = bl_flash_erase(s * FLASH_SECTOR_SIZE);
      _erased[s / 32] |= 1u << s % 32;
    }
  }
  if (ok) ok = bl_flash_program(data, addr, len);
  return ok && !memcmp((const void*)addr, data, len);
}

// --- STK500 ---

static bool _addr_set;      // LOAD_ADDRESS seen in this session
static uint32_t _base_w;    // first word address: APP_START
static uint32_t _cur_w;     // current word address, past 16 bits
static bool _first_ok;      // first page at APP_START written

static void session_reset()
{
  _addr_set = false;
  _first_ok = false;
  memset(_erased, 0, sizeof(_erased));
}

// EdgeTX sends the low 16 bits of the word address, pages in order: follow
// the wrap past 0xFFFF (128 KB)
static void load_address(uint16_t a)
{
  if (!_addr_set) {
    _addr_set = true;
    _base_w = _cur_w = a;
    return;
  }
  uint32_t w = (_cur_w & ~0xFFFFu) | a;
  if (w + 0x8000 < _cur_w) w += 0x10000;
  _cur_w = w;
}

static uint32_t byte_address()
{
  return APP_START + (_cur_w - _base_w) * 2;
}

static uint8_t prog_page(uint8_t* page, uint32_t size)
{
  // EdgeTX: word 0x1000 (256 byte pages), or 0 (128 byte pages)
  if (!_addr_set || (_base_w != 0x1000 && _base_w != 0)) return STK_FAILED;

  uint32_t addr = byte_address();
  if (addr == APP_START) {
    // Before anything is erased: an application for this chip?
    app_header_t* h = size >= APP_HDR_MIN ? header_find(page, size) : 0;
    if (!h || !vectors_ok((const uint32_t*)page)) return STK_FAILED;
    // Not checked until the whole image is there and its CRC matched
    h->checked = 0xFFFFFFFF;
    _first_ok = true;
  } else if (!_first_ok) {
    return STK_FAILED;
  }

  // For the session (main() closes it)
  if (!_flash_open) {
    bl_flash_open();
    _flash_open = true;
  }
  return flash_write(addr, page, size) ? STK_OK : STK_FAILED;
}

static void reply(const uint8_t* data, uint32_t len)
{
  put(STK_INSYNC);
  while (len--) put(*data++);
  put(STK_OK);
}

// Read n argument bytes and CRC_EOP; NOSYNC if the EOP is missing
static bool args(uint8_t* buf, uint32_t n)
{
  uint8_t b;
  for (uint32_t i = 0; i < n; i++) {
    if (!get(&b)) return false;
    if (buf) buf[i] = b;
  }
  if (!get(&b)) return false;
  if (b != CRC_EOP) {
    put(STK_NOSYNC);
    return false;
  }
  return true;
}

// Returns when the application was written and EdgeTX left programming mode
static void serve()
{
  static uint8_t page[PAGE_MAX];
  uint8_t a[20];
  uint32_t last = bl_millis();

  session_reset();
  while (true) {
    uint8_t cmd;
    if (!get(&cmd)) {
      if (bl_millis() - last > BL_IDLE_MS && app_valid()) return;
      continue;
    }
    last = bl_millis();

    switch (cmd) {
      case STK_GET_SYNC:
      case STK_ENTER_PROGMODE:
        if (args(0, 0)) reply(0, 0);
        break;

      case STK_GET_SIGN_ON:
        if (args(0, 0)) reply((const uint8_t*)"AVR STK", 7);
        break;

      case STK_GET_PARAMETER:
        if (args(a, 1)) reply((const uint8_t*)"\0", 1);
        break;

      case STK_SET_DEVICE:
        if (args(0, 20)) reply(0, 0);
        break;

      case STK_READ_SIGN:
        if (args(0, 0)) {
          static const uint8_t sig[] = {BL_SIGNATURE_0, BL_SIGNATURE_1,
                                        BL_SIGNATURE_2};
          reply(sig, sizeof(sig));
        }
        break;

      case STK_LOAD_ADDRESS:
        if (args(a, 2)) {
          load_address(a[0] | a[1] << 8);
          reply(0, 0);
        }
        break;

      case STK_PROG_PAGE: {
        if (!get(&a[0]) || !get(&a[1]) || !get(&a[2])) break;
        uint32_t size = a[0] << 8 | a[1];
        bool fits = size <= PAGE_MAX;
        uint32_t i;
        for (i = 0; i < size && get(fits ? &page[i] : &a[3]); i++) {}
        if (i < size || !args(0, 0)) break;
        put(STK_INSYNC);
        // Memory type: EdgeTX sends 0, avrdude 'F' (flash)
        put(fits && (a[2] == 0 || a[2] == 'F') ? prog_page(page, size)
                                               : STK_FAILED);
        break;
      }

      case STK_READ_PAGE:
        if (args(a, 3)) {
          uint32_t size = a[0] << 8 | a[1];
          uint32_t addr = _addr_set ? byte_address() : APP_START;
          put(STK_INSYNC);
          for (uint32_t i = 0; i < size; i++, addr++)
            put(addr < APP_END ? *(const uint8_t*)addr : 0xFF);
          put(STK_OK);
        }
        break;

      case STK_LEAVE_PROGMODE:
        if (args(0, 0)) {
          reply(0, 0);
          if (app_valid()) return;
          session_reset();
        }
        break;

      default:
        // Unknown length: answer at the next EOP
        do {
          if (!get(&cmd)) break;
        } while (cmd != CRC_EOP);
        if (cmd == CRC_EOP) {
          put(STK_INSYNC);
          put(STK_UNKNOWN);
        }
        break;
    }
  }
}

// Bits only go from 1 to 0: no erase. No system reset: on the CC2652 the
// ROM boot after one waits in a wfi when a debugger is attached (first
// start after a J-Link flash).
static void app_mark_checked()
{
  uint32_t checked = APP_HDR_CHECKED;
  uint32_t addr = (uint32_t)&app_header()->checked;
  bl_flash_open();
  bl_flash_program((const uint8_t*)&checked, addr, sizeof(checked));
  bl_flash_close();
}

int main()
{
  bl_init();
  crc_init();

  bool valid = app_checked();
  if (!valid && app_valid()) {
    app_mark_checked();
    valid = true;
  }

  _n_ports = bl_ports_open();
  for (uint32_t i = 0; i < _n_ports; i++) _radio |= bl_port_radio(i);
  if (!wait_sync(!valid)) app_start();

  bl_port_take(_port);
  reply(0, 0);  // the sync
  serve();

  // New application written, its CRC matched: flash cache back on
  // (invalidated), then marked and started as at power up. No system reset
  // (see app_mark_checked())
  if (_flash_open) bl_flash_close();
  if (!app_checked()) app_mark_checked();
  app_start();
}
