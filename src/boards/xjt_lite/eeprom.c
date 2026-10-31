// The MPM's EEPROM on the XJT-Lite: its 256 byte I2C EEPROM (M24C02 type,
// board.h), bit-banged. The Multiprotocol.h offsets the protocols of this
// build use are mapped to the chip (_map), the others read erased (0xFF)
// and take no writes. A RAM copy of the chip, read at init; eeprom_flush()
// writes the dirty 16 byte pages, one at a time when the next callback
// leaves time for it, the callbacks going on while the chip programs
#include "eeprom.h"

#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <string.h>

#include "board.h"
#include "debug.h"
#include "mpm/mpm.h"
#include "timer.h"
#include "mpm/upstream/mpm_compat.h"

#define CHIP_SIZE 256
#define PAGE_SIZE 16
#define N_PAGES (CHIP_SIZE / PAGE_SIZE)

// A page write on the bus (address, 18 bytes at about 200 kHz), then the
// chip programs for up to 5 ms (M24C02 tW)
#define PAGE_US 1000
#define WRITE_CYCLE_MAX_US 10000

// FrSky D, X and X2 clone data (format, ID, 47 hop channels each), the only
// EEPROM content of this build's protocols
#define CLONE_LEN 51
static const struct {
  uint16_t offset;
  uint16_t len;
  uint8_t chip;
} _map[] = {
    {FRSKYD_CLONE_EEPROM_OFFSET, 3 * CLONE_LEN, 0x00},
};
_Static_assert(FRSKYX_CLONE_EEPROM_OFFSET == FRSKYD_CLONE_EEPROM_OFFSET + CLONE_LEN &&
                   FRSKYX2_CLONE_EEPROM_OFFSET == FRSKYX_CLONE_EEPROM_OFFSET + CLONE_LEN,
               "clone data: one block");

static uint8_t _data[CHIP_SIZE];
static uint16_t _dirty_pages;  // bit n: page n written since its flush
static bool _init, _chip;

// Chip address of an EEPROM offset, -1 if not mapped
static int16_t _chip_addr(uint16_t address)
{
  for (uint8_t i = 0; i < sizeof(_map) / sizeof(_map[0]); i++)
    if (address >= _map[i].offset && address < _map[i].offset + _map[i].len)
      return _map[i].chip + address - _map[i].offset;
  return -1;
}

//
// I2C: open drain by output enable, the outputs low
//

static void _line(uint8_t dio, uint8_t level)
{
  GPIO_setOutputEnableDio(dio, level ? GPIO_OUTPUT_DISABLE : GPIO_OUTPUT_ENABLE);
  delay_us(2);
}

static void _start()
{
  _line(EEPROM_SDA, 1);
  _line(EEPROM_SCL, 1);
  _line(EEPROM_SDA, 0);
  _line(EEPROM_SCL, 0);
}

static void _stop()
{
  _line(EEPROM_SDA, 0);
  _line(EEPROM_SCL, 1);
  _line(EEPROM_SDA, 1);
}

// True if acknowledged
static bool _write_byte(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++, v <<= 1) {
    _line(EEPROM_SDA, v >> 7);
    _line(EEPROM_SCL, 1);
    _line(EEPROM_SCL, 0);
  }
  _line(EEPROM_SDA, 1);
  _line(EEPROM_SCL, 1);
  bool ack = !GPIO_readDio(EEPROM_SDA);
  _line(EEPROM_SCL, 0);
  return ack;
}

static uint8_t _read_byte(bool ack)
{
  uint8_t v = 0;
  _line(EEPROM_SDA, 1);
  for (uint8_t i = 0; i < 8; i++) {
    _line(EEPROM_SCL, 1);
    v = v << 1 | GPIO_readDio(EEPROM_SDA);
    _line(EEPROM_SCL, 0);
  }
  _line(EEPROM_SDA, !ack);
  _line(EEPROM_SCL, 1);
  _line(EEPROM_SCL, 0);
  return v;
}

// Start + the chip's address, write (byte address next) or read
static bool _select(bool read)
{
  _start();
  return _write_byte(EEPROM_I2C_ADDR << 1 | read);
}

static bool _read_all()
{
  bool ok = _select(false) && _write_byte(0) && _select(true);
  if (ok)
    for (uint16_t i = 0; i < CHIP_SIZE; i++) _data[i] = _read_byte(i < CHIP_SIZE - 1);
  _stop();
  return ok;
}

// The chip acknowledges again once the last page is programmed
static bool _wait_ready()
{
  uint32_t start = micros();
  for (;;) {
    bool ready = _select(false);
    _stop();
    if (ready) return true;
    if (micros() - start > WRITE_CYCLE_MAX_US) return false;
    mpm_poll();
  }
}

static bool _write_page(uint8_t page, const uint8_t* data)
{
  bool ok = _select(false) && _write_byte(page * PAGE_SIZE);
  for (uint8_t i = 0; ok && i < PAGE_SIZE; i++) ok = _write_byte(data[i]);
  _stop();
  return ok;
}

void eeprom_init(struct lfs* lfs)
{
  (void)lfs;
  memset(_data, 0xFF, sizeof(_data));
  _dirty_pages = 0;
  _init = true;

  // Released: inputs, pulled up as on the board, the outputs low
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t dio = i ? EEPROM_SCL : EEPROM_SDA;
    GPIO_setOutputEnableDio(dio, GPIO_OUTPUT_DISABLE);
    IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_STD_INPUT | IOC_IOPULL_UP);
    GPIO_clearDio(dio);
  }
  delay_us(10);
  _chip = _read_all();
  if (!_chip) {
    debugln("error: no EEPROM, RAM only");
    memset(_data, 0xFF, sizeof(_data));
  }
}

uint8_t eeprom_read(uint16_t address)
{
  int16_t a = _chip_addr(address);
  return _init && a >= 0 ? _data[a] : 0xFF;
}

void eeprom_write(uint16_t address, uint8_t value)
{
  if (!_init) eeprom_init(0);
  int16_t a = _chip_addr(address);
  if (a < 0 || _data[a] == value) return;
  _data[a] = value;
  _dirty_pages |= 1 << (a / PAGE_SIZE);
}

bool eeprom_dirty() { return _dirty_pages != 0; }

int eeprom_flush()
{
  while (_dirty_pages) {
    // The page's bus transfer before the next callback (protocols that
    // never leave that much: the write goes on anyway)
    uint32_t start = micros();
    while (mpm_time_left() < PAGE_US && micros() - start < WRITE_CYCLE_MAX_US) mpm_poll();

    uint8_t page = __builtin_ctz(_dirty_pages);
    _dirty_pages &= ~(1 << page);
    if (!_chip) continue;
    // The callbacks of the waits may write the page again: dirty again
    uint8_t copy[PAGE_SIZE];
    memcpy(copy, _data + page * PAGE_SIZE, PAGE_SIZE);
    if (!_wait_ready() || !_write_page(page, copy)) {
      debugln("error: EEPROM write failed, RAM only");
      _chip = false;
      return -1;
    }
  }
  return 0;
}
