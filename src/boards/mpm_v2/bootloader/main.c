// MPM v2 boot loader (flash sector 0): the application update from the radio
//
// Speaks the STK500v1 subset EdgeTX uses to flash a MULTI-Module
// (radio/src/io/multi_firmware_update.cpp), at 57600 8N1, on the radio link
// (BL_RADIO_*, board.h) and on the console UART (USB serial). See
// docs/update.md.
//
// At power up: no valid application -> serve STK500 until one is written.
// Otherwise listen for STK_GET_SYNC for up to BL_WINDOW_MS, and start the
// application as soon as the radio link shows normal traffic (serial frames
// or PPM at another rate: framing errors, other bytes) or no radio at all
// (BL_RADIO_ABSENT_LEVEL): a radio rebooting with the model in the air gets
// its module back with its first frames. The full window only runs out with
// the line idle and driven, as EdgeTX does when it flashes (it powers the
// module up and syncs at once for an MPM v2 file).
//
// The image CRC is checked once, at the first start after an update (it
// takes about 0.3 ms per KB): the header's `checked` word is then cleared.

#include <stdbool.h>
#include <string.h>

#include <driverlib/cpu.h>
#include <driverlib/flash.h>
#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>
#include <driverlib/vims.h>
#include <inc/hw_nvic.h>

#include "board.h"
#include "layout.h"

#define BAUDRATE 57600
#define SYS_CLK  48000000

#define BL_WINDOW_MS       300   // longest wait for a sync at power up
#define BL_CONSOLE_MS      20    // shortest wait (host tools reset, then sync)
#define BL_ABSENT_MS       20    // radio line at the "no radio" level
#define BL_NOISE_BYTES     8     // radio traffic that isn't STK500
#define BL_BYTE_TIMEOUT_MS 100   // inside a command
#define BL_IDLE_MS         10000 // session idle with a valid application

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

#define UART_DR_ERRORS 0xF00  // OE, BE, PE, FE in the data register

typedef struct {
  uint32_t base;
  bool radio;
} port_t;

static const port_t _ports[] = {
    {UART0_BASE, false},
#if defined(BL_RADIO_RX)
    {UART1_BASE, true},
#endif
};
#define N_PORTS (sizeof(_ports) / sizeof(_ports[0]))

static const port_t* _port;  // the one that sent the sync

// --- time: SysTick at 1 ms, polled ---

static uint32_t _ms;

static void tick_init()
{
  HWREG(NVIC_ST_RELOAD) = SYS_CLK / 1000 - 1;
  HWREG(NVIC_ST_CURRENT) = 0;
  HWREG(NVIC_ST_CTRL) = NVIC_ST_CTRL_CLK_SRC | NVIC_ST_CTRL_ENABLE;
}

// Ticks are lost while flash is erased: timeouts only get longer
static uint32_t millis()
{
  if (HWREG(NVIC_ST_CTRL) & NVIC_ST_CTRL_COUNT) _ms++;
  return _ms;
}

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

static bool header_ok(const app_header_t* h)
{
  return h->magic == APP_HDR_MAGIC && h->length >= APP_HDR_OFFSET + 0x40 &&
         h->length <= APP_END - APP_START;
}

#define APP_HEADER ((const app_header_t*)(APP_START + APP_HDR_OFFSET))

static bool app_checked()
{
  return vectors_ok((const uint32_t*)APP_START) && header_ok(APP_HEADER) &&
         APP_HEADER->checked == APP_HDR_CHECKED;
}

static bool app_valid()
{
  const uint8_t* img = (const uint8_t*)APP_START;
  const app_header_t* h = APP_HEADER;
  if (!vectors_ok((const uint32_t*)img) || !header_ok(h)) return false;

  static const uint8_t zeros[sizeof(app_header_t)];
  uint32_t crc = 0xFFFFFFFF;
  crc = crc_update(crc, img, APP_HDR_OFFSET);
  crc = crc_update(crc, zeros, sizeof(zeros));
  crc = crc_update(crc, img + APP_HDR_OFFSET + sizeof(zeros),
                   h->length - APP_HDR_OFFSET - sizeof(zeros));
  return ~crc == h->crc;
}

static void ports_close()
{
  for (uint32_t i = 0; i < N_PORTS; i++) {
    while (UARTBusy(_ports[i].base)) {}
    UARTDisable(_ports[i].base);
  }
#if defined(BL_RADIO_TX)
  // Back to the board_init() levels: S.Port released
  IOCPinTypeGpioOutput(BL_RADIO_TX);
  GPIO_writeDio(BL_RADIO_TX, 1);
  #if defined(MODULE_TX_INV)
  GPIO_writeDio(MODULE_TX_INV, 0);
  #endif
#endif
}

static void app_start()
{
  ports_close();
  HWREG(NVIC_ST_CTRL) = 0;
  HWREG(NVIC_VTABLE) = APP_START;

  const uint32_t* vt = (const uint32_t*)APP_START;
  __asm volatile(
      "msr msp, %0\n"
      "bx %1\n" ::"r"(vt[0]),
      "r"(vt[1]));
  while (1) {}
}

// --- UARTs ---

static void ports_open()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
#if defined(BL_RADIO_RX)
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART1);
#endif
  PRCMLoadSet();
  while (!PRCMLoadGet()) {}

  IOCPinTypeUart(UART0_BASE, SERIAL_RX_IOD, SERIAL_TX_IOD, IOID_UNUSED,
                 IOID_UNUSED);
#if defined(BL_RADIO_RX)
  // RX only: TX stays as board_init() set it until the radio sends a sync
  IOCPortConfigureSet(BL_RADIO_RX, IOC_PORT_MCU_UART1_RX,
                      IOC_STD_INPUT | (BL_RADIO_INVERTED ? IOC_IOMODE_INV : 0));
#endif

  for (uint32_t i = 0; i < N_PORTS; i++) {
    uint32_t base = _ports[i].base;
    UARTDisable(base);
    UARTConfigSetExpClk(base, SYS_CLK, BAUDRATE,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                            UART_CONFIG_PAR_NONE);
    UARTEnable(base);
  }
}

static void radio_tx_on()
{
#if defined(BL_RADIO_TX)
  #if defined(MODULE_TX_INV)
  // U9 inverts with TX1_INV set: set it first, the idle GPIO level then
  // holds S.Port at the UART's idle level before the UART takes the pin over
  GPIO_writeDio(MODULE_TX_INV, BL_RADIO_INVERTED);
  IOCPortConfigureSet(BL_RADIO_TX, IOC_PORT_MCU_UART1_TX, IOC_STD_OUTPUT);
  #else
  IOCPortConfigureSet(BL_RADIO_TX, IOC_PORT_MCU_UART1_TX,
                      IOC_STD_OUTPUT | (BL_RADIO_INVERTED ? IOC_IOMODE_INV : 0));
  #endif
#endif
}

static void put(uint8_t b) { UARTCharPut(_port->base, b); }

static bool get(uint8_t* b)
{
  uint32_t start = millis();
  while (millis() - start < BL_BYTE_TIMEOUT_MS) {
    if (UARTCharsAvail(_port->base)) {
      uint32_t v = HWREG(_port->base + UART_O_DR);
      if (v & UART_DR_ERRORS) continue;
      *b = v;
      return true;
    }
  }
  return false;
}

// Power up window: the port that sent a sync, or none (start the application)
static const port_t* wait_sync(bool forever)
{
  uint8_t prev[N_PORTS] = {0};
  uint32_t noise = 0;
  uint32_t start = millis();
#if defined(BL_RADIO_ABSENT_LEVEL)
  uint32_t present = start;
#endif

  while (true) {
    for (uint32_t i = 0; i < N_PORTS; i++) {
      uint32_t base = _ports[i].base;
      while (UARTCharsAvail(base)) {
        uint32_t v = HWREG(base + UART_O_DR);
        uint8_t b = v;
        if (!(v & UART_DR_ERRORS) && prev[i] == STK_GET_SYNC && b == CRC_EOP)
          return &_ports[i];
        if (_ports[i].radio && ((v & UART_DR_ERRORS) || b != STK_GET_SYNC))
          noise++;
        prev[i] = v & UART_DR_ERRORS ? 0 : b;
      }
    }
    if (forever) continue;

    uint32_t t = millis() - start;
    if (t < BL_CONSOLE_MS) continue;
#if defined(BL_RADIO_RX)
    if (noise > BL_NOISE_BYTES) return 0;
  #if defined(BL_RADIO_ABSENT_LEVEL)
    // DIN reads the pad after the IOC inversion
    if ((GPIO_readDio(BL_RADIO_RX) ^ BL_RADIO_INVERTED) != BL_RADIO_ABSENT_LEVEL)
      present = millis();
    else if (millis() - present >= BL_ABSENT_MS)
      return 0;
  #endif
    if (t >= BL_WINDOW_MS) return 0;
#else
    return 0;
#endif
  }
}

// --- flash ---

static uint64_t _erased;  // sectors erased in this session

static bool flash_write(uint32_t addr, uint8_t* data, uint32_t len)
{
  if (addr < APP_START || addr + len > APP_END) return false;

  bool ok = true;
  CPUcpsid();
  for (uint32_t s = addr / FLASH_SECTOR_SIZE;
       ok && s <= (addr + len - 1) / FLASH_SECTOR_SIZE; s++) {
    if (!(_erased & (1ULL << s))) {
      ok = FlashSectorErase(s * FLASH_SECTOR_SIZE) == FAPI_STATUS_SUCCESS;
      _erased |= 1ULL << s;
    }
  }
  if (ok) ok = FlashProgram(data, addr, len) == FAPI_STATUS_SUCCESS;
  CPUcpsie();
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
  _erased = 0;
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
    // Before anything is erased: an MPM v2 application?
    if (size < APP_HDR_OFFSET + sizeof(app_header_t) ||
        !vectors_ok((const uint32_t*)page) ||
        !header_ok((const app_header_t*)(page + APP_HDR_OFFSET)))
      return STK_FAILED;
    // Not checked until the whole image is there and its CRC matched
    ((app_header_t*)(page + APP_HDR_OFFSET))->checked = 0xFFFFFFFF;
    _first_ok = true;
  } else if (!_first_ok) {
    return STK_FAILED;
  }

  if (!_erased) {
    // The cache doesn't see flash writes: off for the session (main() puts
    // it back)
    VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_DISABLED, true);
    VIMSLineBufDisable(VIMS_BASE);
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
  uint32_t last = millis();

  session_reset();
  while (true) {
    uint8_t cmd;
    if (!get(&cmd)) {
      if (millis() - last > BL_IDLE_MS && app_valid()) return;
      continue;
    }
    last = millis();

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

// Bits only go from 1 to 0: no erase. The cache doesn't see flash writes:
// off for the write, back through OFF (invalidated, VIMSModeSafeSet()). No
// system reset: the ROM boot after one waits in a wfi when a debugger is
// attached (first start after a J-Link flash).
static void app_mark_checked()
{
  uint32_t checked = APP_HDR_CHECKED;
  uint32_t mode = VIMSModeGet(VIMS_BASE);
  CPUcpsid();
  VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_DISABLED, true);
  VIMSLineBufDisable(VIMS_BASE);
  FlashProgram((uint8_t*)&checked, (uint32_t)&APP_HEADER->checked,
               sizeof(checked));
  VIMSLineBufEnable(VIMS_BASE);
  VIMSModeSafeSet(VIMS_BASE, mode, true);
  CPUcpsie();
}

int main()
{
  board_init();
  tick_init();
  crc_init();

  bool valid = app_checked();
  if (!valid && app_valid()) {
    app_mark_checked();
    valid = true;
  }

  ports_open();
  _port = wait_sync(!valid);
  if (!_port) app_start();

  if (_port->radio) radio_tx_on();
  uint32_t vims = VIMSModeGet(VIMS_BASE);
  reply(0, 0);  // the sync
  serve();

  // New application written, its CRC matched: cache back on (invalidated),
  // then marked and started as at power up. No system reset (see
  // app_mark_checked())
  VIMSModeSafeSet(VIMS_BASE, vims, true);
  VIMSLineBufEnable(VIMS_BASE);
  if (!app_checked()) app_mark_checked();
  app_start();
}
