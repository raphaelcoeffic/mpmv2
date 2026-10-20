// The file system (lfs_driver.c), the EEPROM file (eeprom.c), the file
// server and the settings files on a simulated NOR flash: format, an older
// smaller file system grown, power cuts during a flush or an upload, a
// failing block, callbacks writing during a flush, wear over many flushes
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eeprom.h"
#include "file_server.h"
#include "file_system.h"
#include "flash_driver.h"
#include "mpm/mpm.h"
#include "mpm/mpm_settings.h"
#include "nor_flash.h"
#include "settings.h"
#include "log.h"

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

//
// File server: requests as the host sends them
//

// zlib's CRC-32, bit by bit (file_server.c uses littlefs's table)
static uint32_t _crc32(uint32_t crc, const uint8_t* d, uint32_t n)
{
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (int k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
  }
  return ~crc;
}

static uint8_t _seq;
static uint8_t _reply[FILE_SERVER_REPLY_MAX];
static uint8_t _reply_len;
static uint8_t _last_req[FILE_SERVER_REQ_MAX];
static uint8_t _last_req_len;

static void _put32(uint8_t* p, uint32_t v)
{
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
static uint32_t _get32(const uint8_t* p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// A request (a new seq), its status; the reply payload in _reply + 3
static int _req(uint8_t op, const void* payload, uint32_t n)
{
  uint8_t* r = _last_req;
  r[0] = ++_seq;
  r[1] = op;
  memcpy(r + 2, payload, n);
  _put32(r + 2 + n, _crc32(0, r, 2 + n));
  _last_req_len = 6 + n;
  _reply_len = file_server_request(r, _last_req_len, _reply);
  if (_reply_len < 7) return 1000;
  CHECK(_reply[0] == r[0] && _reply[1] == op);
  CHECK(_get32(_reply + _reply_len - 4) == _crc32(0, _reply, _reply_len - 4));
  return (int8_t)_reply[2];
}

static int _req_path(uint8_t op, const char* path)
{
  return _req(op, path, strlen(path));
}

static int _put(const char* path, const uint8_t* data, uint32_t size, uint32_t crc)
{
  uint8_t p[8 + FILE_SERVER_PATH_MAX];
  _put32(p, size);
  _put32(p + 4, crc);
  memcpy(p + 8, path, strlen(path));
  int err = _req(FILE_SERVER_OPEN, p, 8 + strlen(path));
  if (err) return err;
  for (uint32_t off = 0; off < size; off += FILE_SERVER_DATA_MAX) {
    uint32_t n = size - off < FILE_SERVER_DATA_MAX ? size - off : FILE_SERVER_DATA_MAX;
    uint8_t w[4 + FILE_SERVER_DATA_MAX];
    _put32(w, off);
    memcpy(w + 4, data + off, n);
    err = _req(FILE_SERVER_WRITE, w, 4 + n);
    if (err) return err;
  }
  return _req(FILE_SERVER_CLOSE, 0, 0);
}

// Whole file into out, its size or an error
static int _get(const char* path, uint8_t* out, uint32_t max)
{
  uint32_t off = 0;
  for (;;) {
    uint8_t p[5 + FILE_SERVER_PATH_MAX];
    _put32(p, off);
    p[4] = FILE_SERVER_DATA_MAX;
    memcpy(p + 5, path, strlen(path));
    int err = _req(FILE_SERVER_READ, p, 5 + strlen(path));
    if (err) return err;
    uint32_t n = _reply_len - 7;
    if (off + n > max) return -1000;
    memcpy(out + off, _reply + 3, n);
    off += n;
    if (n < FILE_SERVER_DATA_MAX) return off;
  }
}

static uint8_t _file[5000], _back[5000];

static void _random(uint8_t* d, uint32_t n, int seed)
{
  srand(seed);
  for (uint32_t i = 0; i < n; i++) d[i] = rand();
}

static void test_file_server()
{
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  file_server_init(&_lfs, 0);

  CHECK(_req(FILE_SERVER_INFO, 0, 0) == 0 && _reply_len == 7 + 12);
  CHECK(_get32(_reply + 3) == 4096 && _get32(_reply + 7) == SECTORS && _get32(_reply + 11) >= 2);

  // upload, read back
  CHECK(_req_path(FILE_SERVER_MKDIR, "tables") == 0);
  CHECK(_req_path(FILE_SERVER_MKDIR, "tables") == LFS_ERR_EXIST);
  _random(_file, 4321, 1);
  CHECK(_put("tables/t1.bin", _file, 4321, _crc32(0, _file, 4321)) == 0);
  CHECK(_req_path(FILE_SERVER_STAT, "tables/t1.bin") == 0);
  CHECK(_reply[3] == 1 && _get32(_reply + 4) == 4321);
  CHECK(_get("tables/t1.bin", _back, sizeof(_back)) == 4321 && memcmp(_file, _back, 4321) == 0);
  // an empty file
  CHECK(_put("empty", _file, 0, 0) == 0);
  CHECK(_get("empty", _back, sizeof(_back)) == 0);

  // a retried request (lost reply): the same reply, not run again
  CHECK(_req_path(FILE_SERVER_REMOVE, "empty") == 0);
  uint8_t again[FILE_SERVER_REPLY_MAX];
  CHECK(file_server_request(_last_req, _last_req_len, again) == _reply_len);
  CHECK(again[2] == 0);
  CHECK(_req_path(FILE_SERVER_STAT, "empty") == LFS_ERR_NOENT);
  // retried WRITEs during an upload
  _random(_file, 1000, 2);
  uint8_t p[8 + 32];
  _put32(p, 1000);
  _put32(p + 4, _crc32(0, _file, 1000));
  memcpy(p + 8, "retry", 5);
  CHECK(_req(FILE_SERVER_OPEN, p, 13) == 0);
  for (uint32_t off = 0; off < 1000; off += 100) {
    uint8_t w[104];
    _put32(w, off);
    memcpy(w + 4, _file + off, 100);
    CHECK(_req(FILE_SERVER_WRITE, w, 104) == 0);
    CHECK(file_server_request(_last_req, _last_req_len, again) == _reply_len && again[2] == 0);
  }
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == 0);
  CHECK(_get("retry", _back, sizeof(_back)) == 1000 && memcmp(_file, _back, 1000) == 0);

  // a bad frame CRC: no reply
  uint8_t bad[8] = {++_seq, FILE_SERVER_INFO, 1, 2, 3, 4};
  CHECK(file_server_request(bad, 6, _reply) == 0);

  // wrong content CRC, short upload, out of order WRITE: the old file kept
  _random(_back, 4321, 3);
  CHECK(_put("tables/t1.bin", _back, 4321, 1234) == LFS_ERR_CORRUPT);
  _put32(p, 4321);
  _put32(p + 4, _crc32(0, _back, 4321));
  memcpy(p + 8, "tables/t1.bin", 13);
  CHECK(_req(FILE_SERVER_OPEN, p, 21) == 0);
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == LFS_ERR_CORRUPT);
  CHECK(_req(FILE_SERVER_OPEN, p, 21) == 0);
  uint8_t w[4 + 16];
  _put32(w, 16);
  CHECK(_req(FILE_SERVER_WRITE, w, sizeof(w)) == LFS_ERR_INVAL);
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == LFS_ERR_CORRUPT);
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == LFS_ERR_BADF);
  _random(_file, 4321, 1);
  CHECK(_get("tables/t1.bin", _back, sizeof(_back)) == 4321 && memcmp(_file, _back, 4321) == 0);
  CHECK(_req_path(FILE_SERVER_STAT, FILE_SERVER_UPLOAD) == LFS_ERR_NOENT);

  // rename, remove, errors
  char ren[] = "retry\0tables/t2.bin";
  CHECK(_req(FILE_SERVER_RENAME, ren, sizeof(ren) - 1) == 0);
  CHECK(_req_path(FILE_SERVER_STAT, "retry") == LFS_ERR_NOENT);
  CHECK(_req_path(FILE_SERVER_REMOVE, "tables") == LFS_ERR_NOTEMPTY);
  CHECK(_req_path(FILE_SERVER_READ, "x") == LFS_ERR_INVAL);
  CHECK(_req(99, 0, 0) == LFS_ERR_INVAL);

  // an EEPROM flush during an upload: both files fine
  _random(_file, 3000, 4);
  _put32(p, 3000);
  _put32(p + 4, _crc32(0, _file, 3000));
  memcpy(p + 8, "big", 3);
  CHECK(_req(FILE_SERVER_OPEN, p, 11) == 0);
  for (uint32_t off = 0; off < 3000; off += 100) {
    uint8_t w2[104];
    _put32(w2, off);
    memcpy(w2 + 4, _file + off, 100);
    CHECK(_req(FILE_SERVER_WRITE, w2, 104) == 0);
    if (off == 1500) {
      _fill(7);
      CHECK(eeprom_flush() == 0);
    }
  }
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == 0);
  CHECK(_boot() && _is(7));
  file_server_init(&_lfs, 0);
  CHECK(_get("big", _back, sizeof(_back)) == 3000 && memcmp(_file, _back, 3000) == 0);

  // the EEPROM file rewritten between two READs of it (READ keeps the file
  // open: file_server_sync() before the flush, as mpm_port_idle()): the
  // next READ reads the new content
  _fill(8);
  CHECK(eeprom_flush() == 0);
  uint8_t r[5 + 6] = {0, 0, 0, 0, 128, 'e', 'e', 'p', 'r', 'o', 'm'};
  CHECK(_req(FILE_SERVER_READ, r, sizeof(r)) == 0 && _reply_len == 7 + 128);
  CHECK(_reply[3 + 5] == (uint8_t)(5 * 7 + 8));
  _fill(9);
  file_server_sync();
  CHECK(eeprom_flush() == 0);
  r[0] = 128;
  CHECK(_req(FILE_SERVER_READ, r, sizeof(r)) == 0 && _reply_len == 7 + 128);
  CHECK(_reply[3 + 5] == (uint8_t)(133 * 7 + 9));
  r[0] = 0;
  CHECK(_req(FILE_SERVER_READ, r, sizeof(r)) == 0 && _reply[3 + 5] == (uint8_t)(5 * 7 + 9));

  // a listing over several replies
  for (int i = 0; i < 30; i++) {
    char name[64];
    snprintf(name, sizeof(name), "tables/a_rather_long_table_name_%02d.bin", i);
    CHECK(_put(name, (const uint8_t*)name, i, _crc32(0, (const uint8_t*)name, i)) == 0);
  }
  int seen = 0, pages = 0, sizes = 0;
  for (uint16_t index = 0;; pages++) {
    uint8_t l[2 + 16] = {index, index >> 8};
    memcpy(l + 2, "tables", 6);
    CHECK(_req(FILE_SERVER_LIST, l, 8) == 0);
    uint32_t i = 4, end = _reply_len - 4;
    int n = 0;
    while (i < end) {
      sizes += _get32(_reply + i + 1);
      i += 6 + _reply[i + 5];
      n++;
    }
    CHECK(i == end);
    seen += n;
    index += n;
    if (!_reply[3]) break;
    CHECK(n > 0);
  }
  printf("file server: list of 32 entries in %d replies\n", pages + 1);
  CHECK(seen == 32 && pages > 0 && sizes == 4321 + 1000 + 29 * 30 / 2);
}

static void test_upload_power_cut()
{
  // a cut at each program / erase of an upload replacing a file: the old
  // file after the boot, no upload left
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  file_server_init(&_lfs, 0);
  _random(_file, 2000, 5);
  CHECK(_put("table", _file, 2000, _crc32(0, _file, 2000)) == 0);
  memcpy(_saved, _mem, sizeof(_mem));
  _random(_back, 2500, 6);
  int cuts = 0, old = 0;
  for (long cut = 0;; cut++) {
    memcpy(_mem, _saved, sizeof(_mem));
    CHECK(_boot());
    file_server_init(&_lfs, 0);
    _ops_left = cut;
    if (setjmp(_cut) == 0) {
      CHECK(_put("table", _back, 2500, _crc32(0, _back, 2500)) == 0);
      _ops_left = -1;
      break;
    }
    _ops_left = -1;
    cuts++;
    CHECK(_boot());
    file_server_init(&_lfs, 0);
    static uint8_t got[3000];
    int n = _get("table", got, sizeof(got));
    if (n == 2000 && memcmp(got, _file, 2000) == 0) old++;
    else CHECK(n == 2500 && memcmp(got, _back, 2500) == 0);
    CHECK(_req_path(FILE_SERVER_STAT, FILE_SERVER_UPLOAD) == LFS_ERR_NOENT);
  }
  printf("upload power cut: %d cuts, old file %d, new %d\n", cuts, old, cuts - old);
  CHECK(cuts > 5);
}

//
// Settings: ppm.txt through the file server
//

// The protocols the settings name (the firmware's: protocols.c)
const mpm_protocol_t mpm_protocols[] = {
    {64, "FrSkyX2", "\x07" "D16\0   " "D16 8ch" "LBT(EU)" "LBT 8ch", 4, 0, true, false, 0, 0, 0},
    {28, "FlSky2A", "\x08" "PWM,IBUS" "PPM,IBUS" "PWM,SBUS" "PPM,SBUS", 4, 0, true, true, 0, 0, 0},
};
const uint8_t mpm_protocol_count = 2;

static bool _err_file(const char* path, char* out, uint32_t size)
{
  static uint8_t buf[600];
  int n = _get(path, buf, sizeof(buf));
  if (n < 0) return false;
  if ((uint32_t)n >= size) n = size - 1;
  memcpy(out, buf, n);
  out[n] = 0;
  return true;
}

static bool _ppm_err(char* out, uint32_t size) { return _err_file(SETTINGS_PPM_ERR, out, size); }

static int _put_text(const char* path, const char* text)
{
  return _put(path, (const uint8_t*)text, strlen(text), _crc32(0, (const uint8_t*)text, strlen(text)));
}

static void test_settings()
{
  memset(_mem, 0xFF, sizeof(_mem));
  CHECK(_boot());
  settings_init(&_lfs);
  file_server_init(&_lfs, settings_file_changed);
  mpm_ppm_settings_t def;
  mpm_ppm_defaults(&def);
  char err[600];
  CHECK(memcmp(&mpm_ppm, &def, sizeof(def)) == 0 && !_ppm_err(err, sizeof(err)));

  // uploaded: applied at once
  CHECK(_put_text("ppm.txt", "protocol = FlSky2A\nsub = PWM,SBUS\nrx_num = 2\nautobind = 1\n") == 0);
  CHECK(mpm_ppm.protocol == 28 && mpm_ppm.sub_protocol == 2 && mpm_ppm.rx_num == 2 && mpm_ppm.autobind);
  CHECK(!_ppm_err(err, sizeof(err)));

  // bad lines: their defaults, ppm.err; at power up again
  CHECK(_put_text("/ppm.txt", "protocol = FlSky2A\nrx_num = 99\n") == 0);
  CHECK(mpm_ppm.protocol == 28 && mpm_ppm.rx_num == 0 && !mpm_ppm.autobind);
  CHECK(_ppm_err(err, sizeof(err)) && strcmp(err, "line 2: rx_num: 0 to 63: 99\n") == 0);
  mpm_ppm_defaults(&mpm_ppm);
  CHECK(_boot());
  settings_init(&_lfs);
  file_server_init(&_lfs, settings_file_changed);
  CHECK(mpm_ppm.protocol == 28 && _ppm_err(err, sizeof(err)));

  // fixed: ppm.err removed
  CHECK(_put_text("ppm.txt", "# FrSky X2 EU\nprotocol = frskyx2\nsub = lbt(eu)\n") == 0);
  CHECK(mpm_ppm.protocol == 64 && mpm_ppm.sub_protocol == 2 && !_ppm_err(err, sizeof(err)));

  // removed: the defaults; renamed into place: applied
  CHECK(_req_path(FILE_SERVER_REMOVE, "ppm.txt") == 0);
  CHECK(memcmp(&mpm_ppm, &def, sizeof(def)) == 0);
  CHECK(_put_text("next.txt", "rx_num = 7\n") == 0);
  CHECK(mpm_ppm.rx_num == 0);
  char ren[] = "next.txt\0ppm.txt";
  CHECK(_req(FILE_SERVER_RENAME, ren, sizeof(ren) - 1) == 0);
  CHECK(mpm_ppm.rx_num == 7);

  // too long: what was read, and a message
  static char longtext[1500];
  memset(longtext, ' ', sizeof(longtext) - 1);
  memcpy(longtext, "rx_num = 9\n", 11);
  longtext[sizeof(longtext) - 1] = 0;
  CHECK(_put_text("ppm.txt", longtext) == 0);
  CHECK(mpm_ppm.rx_num == 9);
  CHECK(_ppm_err(err, sizeof(err)) && strstr(err, "read up to 1024 bytes only\n"));

  // debug.txt: the log sent from power up
  CHECK(!(log_on & LOG_FILE));
  CHECK(_put_text("debug.txt", "# log on USB\nusb = on\n") == 0);
  CHECK((log_on & LOG_FILE) && !_err_file(SETTINGS_DEBUG_ERR, err, sizeof(err)));
  CHECK(_boot());
  log_enable(LOG_FILE, false);
  settings_init(&_lfs);
  CHECK(log_on & LOG_FILE);
  CHECK(_put_text("debug.txt", "usb = maybe\nlevel = 3\n") == 0);
  CHECK(!(log_on & LOG_FILE) && _err_file(SETTINGS_DEBUG_ERR, err, sizeof(err)));
  CHECK(strcmp(err, "line 1: usb: on or off: maybe\nline 2: unknown key: level\n") == 0);
  CHECK(_req_path(FILE_SERVER_REMOVE, "debug.txt") == 0);
  CHECK(!(log_on & LOG_FILE) && !_err_file(SETTINGS_DEBUG_ERR, err, sizeof(err)));

  // no file system: the defaults
  settings_init(0);
  mpm_ppm_defaults(&mpm_ppm);
  settings_file_changed("ppm.txt");
  CHECK(memcmp(&mpm_ppm, &def, sizeof(def)) == 0);
  file_server_init(&_lfs, 0);
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
  test_file_server();
  test_upload_power_cut();
  test_settings();
  test_ram_only();
  printf("%s\n", _fails ? "FAILED" : "ok");
  return _fails != 0;
}
