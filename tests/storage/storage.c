// The file system (lfs_driver.c) and the EEPROM file (eeprom.c) on a
// simulated NOR flash: format, an older smaller file system grown, power
// cuts during a flush, a failing block, callbacks writing during a flush,
// wear over many flushes
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eeprom.h"
#include "file_system.h"
#include "flash_driver.h"
#include "nor_flash.h"

static int _fails;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
      _fails++;                                                        \
    }                                                                  \
  } while (0)

//
// Simulated NOR flash (MX25R8035F): a program clears bits, an erase sets a
// sector to 0xFF. A power cut after a number of program / erase operations
// leaves the last one half done and jumps back to the test
//

#define FLASH_SIZE (1024 * 1024)
#define SECTORS (FLASH_SIZE / NOR_FLASH_SECTOR_SIZE)

static uint8_t _mem[FLASH_SIZE];
static uint32_t _erases[SECTORS];
static int _bad_sector = -1;  // program fails there
static long _ops_left = -1;   // -1: no power cut
static jmp_buf _cut;
static long _erase_n;
static void (*_yield)();
static long _yields;

int flash_init() { return 0; }
int nor_flash_init(spi_t spi, const spi_device_t* dev) { return 0; }
void nor_flash_set_yield(void (*yield)()) { _yield = yield; }
uint32_t nor_flash_size() { return FLASH_SIZE; }
void nor_flash_sync() {}

static void _wait()
{
  _yields++;
  if (_yield) _yield();
}

static bool _power_cut()
{
  if (_ops_left < 0) return false;
  return _ops_left-- == 0;
}

uint32_t nor_flash_read(uint32_t addr, uint8_t* data, uint32_t len)
{
  memcpy(data, _mem + addr, len);
  _wait();
  return len;
}

int nor_flash_write(uint32_t addr, const uint8_t* data, uint32_t len)
{
  if (addr + len > FLASH_SIZE) abort();
  if (_power_cut()) {
    for (uint32_t i = 0; i < len / 2; i++) _mem[addr + i] &= data[i];
    longjmp(_cut, 1);
  }
  if ((int)(addr / NOR_FLASH_SECTOR_SIZE) == _bad_sector) {
    _mem[addr] &= 0x5A;
    return -1;
  }
  for (uint32_t i = 0; i < len; i++) _mem[addr + i] &= data[i];
  _wait();
  return 0;
}

int nor_flash_erase(uint32_t addr)
{
  if (addr % NOR_FLASH_SECTOR_SIZE) return -1;
  if (_power_cut()) {
    // part of the sector erased, the rest as it was
    for (uint32_t i = 0; i < NOR_FLASH_SECTOR_SIZE; i += 2) _mem[addr + i] = 0xFF;
    longjmp(_cut, 1);
  }
  memset(_mem + addr, 0xFF, NOR_FLASH_SECTOR_SIZE);
  _erases[addr / NOR_FLASH_SECTOR_SIZE]++;
  _erase_n++;
  _wait();
  return 0;
}

//
// Helpers
//

static lfs_t _lfs;

// Power up: mount (format if needed), load the EEPROM
static bool _boot()
{
  int err = file_system_init(&_lfs);
  CHECK(err == 0);
  eeprom_init(err == 0 ? &_lfs : 0);
  return err == 0;
}

static void _fill(uint8_t seed)
{
  for (int i = 0; i < EEPROM_SIZE; i++) eeprom_write(i, (uint8_t)(i * 7 + seed));
}

static bool _is(uint8_t seed)
{
  for (int i = 0; i < EEPROM_SIZE; i++)
    if (eeprom_read(i) != (uint8_t)(i * 7 + seed)) return false;
  return true;
}

static bool _erased()
{
  for (int i = 0; i < EEPROM_SIZE; i++)
    if (eeprom_read(i) != 0xFF) return false;
  return true;
}

//
// Tests
//

static void test_blank()
{
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  struct lfs_fsinfo info;
  CHECK(lfs_fs_stat(&_lfs, &info) == 0 && info.block_count == SECTORS);
  CHECK(_erased() && !eeprom_dirty());

  // upstream: the CYRF ID, then its flag
  for (int i = 0; i < 6; i++) eeprom_write(1 + i, 0x10 + i);
  eeprom_write(0, 0xF0);
  CHECK(eeprom_dirty());
  CHECK(eeprom_flush() == 0 && !eeprom_dirty());
  // the same value again: nothing to write
  eeprom_write(0, 0xF0);
  CHECK(!eeprom_dirty());

  CHECK(_boot());
  CHECK(eeprom_read(0) == 0xF0 && eeprom_read(1) == 0x10 && eeprom_read(6) == 0x15);
  CHECK(eeprom_read(7) == 0xFF && eeprom_read(EEPROM_SIZE) == 0xFF);
}

static void test_garbage()
{
  // foreign data: formatted
  srand(3);
  for (int i = 0; i < FLASH_SIZE; i++) _mem[i] = rand();
  CHECK(_boot());
  CHECK(_erased());
}

// The 2025 test file system: 256 KB, 16 byte caches
static uint8_t _old_rcache[16], _old_pcache[16], _old_lookahead[16];
static int _old_read(const struct lfs_config* c, lfs_block_t b, lfs_off_t o, void* buf, lfs_size_t n)
{
  nor_flash_read(b * c->block_size + o, buf, n);
  return 0;
}
static int _old_prog(const struct lfs_config* c, lfs_block_t b, lfs_off_t o, const void* buf,
                     lfs_size_t n)
{
  return nor_flash_write(b * c->block_size + o, buf, n);
}
static int _old_erase(const struct lfs_config* c, lfs_block_t b)
{
  return nor_flash_erase(b * c->block_size);
}
static int _old_sync(const struct lfs_config* c) { return 0; }

static void test_grow()
{
  memset(_mem, 0xFF, sizeof(_mem));
  const struct lfs_config old = {
      .read = _old_read, .prog = _old_prog, .erase = _old_erase, .sync = _old_sync,
      .read_size = 16, .prog_size = 16, .block_size = 4096, .block_count = 64,
      .cache_size = 16, .lookahead_size = 16, .block_cycles = 500,
      .read_buffer = _old_rcache, .prog_buffer = _old_pcache, .lookahead_buffer = _old_lookahead,
  };
  lfs_t lfs;
  CHECK(lfs_format(&lfs, &old) == 0);
  CHECK(lfs_mount(&lfs, &old) == 0);
  static uint8_t buf[16];
  struct lfs_file_config fcfg = {.buffer = buf};
  lfs_file_t f;
  CHECK(lfs_file_opencfg(&lfs, &f, "lorem_ipsum", LFS_O_WRONLY | LFS_O_CREAT, &fcfg) == 0);
  CHECK(lfs_file_write(&lfs, &f, "Sed ut perspiciatis", 19) == 19);
  CHECK(lfs_file_close(&lfs, &f) == 0);
  CHECK(lfs_unmount(&lfs) == 0);

  CHECK(_boot());
  struct lfs_fsinfo info;
  CHECK(lfs_fs_stat(&_lfs, &info) == 0 && info.block_count == SECTORS);
  struct lfs_info st;
  CHECK(lfs_stat(&_lfs, "lorem_ipsum", &st) == 0 && st.size == 19);
  _fill(1);
  CHECK(eeprom_flush() == 0);
  CHECK(_boot() && _is(1));
}

static uint8_t _saved[FLASH_SIZE];

static void test_power_cut()
{
  // A persisted, B being flushed: a cut after each program / erase of the
  // flush, each from the same flash content. The file system one flush
  // further each round: some flushes compact or relocate the metadata.
  // After the cut: A or B, never a mix, and the next flush works
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  int cuts = 0, old = 0, new = 0, ops_max = 0, compactions = 0;
  for (int round = 0; round < 300; round++) {
    uint8_t a = round & 1 ? 1 : 5;
    _fill(a);
    CHECK(eeprom_flush() == 0);
    memcpy(_saved, _mem, sizeof(_mem));
    for (long cut = 0;; cut++) {
      memcpy(_mem, _saved, sizeof(_mem));
      CHECK(_boot() && _is(a));
      _fill(2);
      _ops_left = cut;
      long erases = _erase_n;
      if (setjmp(_cut) == 0) {
        CHECK(eeprom_flush() == 0);
        _ops_left = -1;
        if (cut > ops_max) ops_max = cut;
        if (_erase_n - erases > 1) compactions++;
        CHECK(_boot() && _is(2));
        break;  // the flush finished before the cut
      }
      _ops_left = -1;
      cuts++;
      CHECK(_boot());
      bool is_a = _is(a), is_b = _is(2);
      CHECK(is_a || is_b);
      if (is_a) old++;
      if (is_b) new++;
      _fill(3);
      CHECK(eeprom_flush() == 0);
      CHECK(_boot() && _is(3));
    }
    memcpy(_mem, _saved, sizeof(_mem));
    CHECK(_boot());
  }
  printf("power cut: %d cuts (flushes of up to %d programs and erases, %d with more than one "
         "erase), old content %d, new %d\n",
         cuts, ops_max, compactions, old, new);
  CHECK(cuts > 1000 && compactions > 0 && old + new == cuts);
}

static void test_bad_block()
{
  // the block of the file fails to program: littlefs moves it
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  _fill(1);
  CHECK(eeprom_flush() == 0);
  for (int k = 0; k < 20; k++) {
    _bad_sector = 2 + k;
    _fill(10 + k);
    CHECK(eeprom_flush() == 0);
  }
  _bad_sector = -1;
  CHECK(_boot() && _is(29));
}

// A callback writing the EEPROM in the flash waits
static int _yield_writes;
static void _yield_write()
{
  if (_yield_writes > 0 && --_yield_writes == 0) eeprom_write(100, 0x42);
}

static void test_write_during_flush()
{
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  _fill(1);
  nor_flash_set_yield(_yield_write);
  _yield_writes = 5;
  CHECK(eeprom_flush() == 0);
  // the flush wrote its copy, the new byte waits for the next one
  CHECK(eeprom_dirty() && eeprom_read(100) == 0x42);
  nor_flash_set_yield(0);
  CHECK(_boot() && _is(1));
  _fill(1);
  eeprom_write(100, 0x42);
  CHECK(eeprom_flush() == 0);
  CHECK(_boot() && eeprom_read(100) == 0x42 && eeprom_read(99) == (uint8_t)(99 * 7 + 1));
}

static void test_wear()
{
  // A bind writes a few bytes: one flush each
  memset(_mem, 0xFF, sizeof(_mem));
  memset(_erases, 0, sizeof(_erases));
  CHECK(_boot());
  const int flushes = 100000;
  long yields = _yields;
  for (int k = 0; k < flushes; k++) {
    eeprom_write(50 + (k % 64), k);
    CHECK(eeprom_flush() == 0);
  }
  uint32_t max = 0, total = 0;
  for (int i = 0; i < SECTORS; i++) {
    total += _erases[i];
    if (_erases[i] > max) max = _erases[i];
  }
  printf("wear: %d flushes, %.2f erases and %.1f flash operations each, sector erases max %u "
         "mean %.0f\n",
         flushes, (double)total / flushes, (double)(_yields - yields) / flushes, max,
         (double)total / (double)SECTORS);
  // 100000 cycles a sector: bound by the mean, not a hot spot
  CHECK(max < 2 * total / SECTORS + 10);
  CHECK(_boot());
  CHECK(eeprom_read(50 + (flushes - 1) % 64) == (uint8_t)(flushes - 1));
}

static void test_ram_only()
{
  eeprom_init(0);
  CHECK(_erased());
  eeprom_write(10, 1);
  CHECK(eeprom_dirty() && eeprom_flush() == 0 && !eeprom_dirty());
  CHECK(eeprom_read(10) == 1);
}

int main()
{
  test_blank();
  test_garbage();
  test_grow();
  test_power_cut();
  test_bad_block();
  test_write_during_flush();
  test_wear();
  test_ram_only();
  printf("%s\n", _fails ? "FAILED" : "ok");
  return _fails != 0;
}
