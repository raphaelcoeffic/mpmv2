// The file "eeprom" without a file system (file_server_ram.c): listed, read,
// uploaded (a shorter file, a bad one, an abandoned one), removed; the other
// paths and requests refused as mpm_fs.py expects
#include <string.h>

#include "eeprom.h"
#include "file_client.h"
#include "file_server.h"
#include "log.h"
#include "test.h"

static const char* _changed_path;
static int _changes;

static void _changed(const char* path)
{
  _changed_path = path;
  _changes++;
}

static uint8_t _img[EEPROM_SIZE + 1], _back[EEPROM_SIZE + 200];

static uint32_t _crc(uint32_t n) { return _crc32(0, _img, n); }

static int _open(const char* path, uint32_t size, uint32_t crc)
{
  uint8_t p[8 + FILE_SERVER_PATH_MAX];
  _put32(p, size);
  _put32(p + 4, crc);
  memcpy(p + 8, path, strlen(path));
  return _req(FILE_SERVER_OPEN, p, 8 + strlen(path));
}

static void test_list_stat()
{
  // "/": the file, its size
  uint8_t p[3] = {0, 0, '/'};
  CHECK(_req(FILE_SERVER_LIST, p, 3) == 0 && _reply_len == 7 + 1 + 6 + 6);
  CHECK(_reply[3] == 0 && _reply[4] == 1 && _get32(_reply + 5) == EEPROM_SIZE && _reply[9] == 6);
  CHECK(memcmp(_reply + 10, "eeprom", 6) == 0);
  p[0] = 1;
  CHECK(_req(FILE_SERVER_LIST, p, 3) == 0 && _reply_len == 7 + 1 && _reply[3] == 0);
  uint8_t dir[] = {0, 0, 't', 'a', 'b'};
  CHECK(_req(FILE_SERVER_LIST, dir, sizeof(dir)) == -2);

  CHECK(_req_path(FILE_SERVER_STAT, "/eeprom") == 0 && _reply[3] == 1);
  CHECK(_get32(_reply + 4) == EEPROM_SIZE);
  CHECK(_req_path(FILE_SERVER_STAT, "/") == 0 && _reply[3] == 2);
  CHECK(_req_path(FILE_SERVER_STAT, "ppm.txt") == -2);
  CHECK(_req_path(FILE_SERVER_STAT, "eeprom2") == -2);
  CHECK(_req(FILE_SERVER_INFO, 0, 0) == -5);  // no file system
}

static void test_read_write()
{
  // erased at power up, read in pieces
  CHECK(_get("eeprom", _back, sizeof(_back)) == EEPROM_SIZE);
  int erased = 1;
  for (int i = 0; i < EEPROM_SIZE; i++) erased &= _back[i] == 0xFF;
  CHECK(erased);
  CHECK(_get("ppm.txt", _back, sizeof(_back)) == -2);
  CHECK(_get("/", _back, sizeof(_back)) == -21);

  // a whole image: written at CLOSE only
  for (int i = 0; i < EEPROM_SIZE; i++) _img[i] = i * 3 + 1;
  CHECK(_open("eeprom", EEPROM_SIZE, _crc(EEPROM_SIZE)) == 0);
  uint8_t w[4 + FILE_SERVER_DATA_MAX];
  _put32(w, 0);
  memcpy(w + 4, _img, FILE_SERVER_DATA_MAX);
  CHECK(_req(FILE_SERVER_WRITE, w, sizeof(w)) == 0);
  CHECK(eeprom_read(0) == 0xFF && _changes == 0);
  // out of order: refused, the upload goes on
  _put32(w, 2 * FILE_SERVER_DATA_MAX);
  CHECK(_req(FILE_SERVER_WRITE, w, sizeof(w)) == -22);
  for (uint32_t off = FILE_SERVER_DATA_MAX; off < EEPROM_SIZE; off += FILE_SERVER_DATA_MAX) {
    _put32(w, off);
    memcpy(w + 4, _img + off, FILE_SERVER_DATA_MAX);
    CHECK(_req(FILE_SERVER_WRITE, w, sizeof(w)) == 0);
  }
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == 0);
  CHECK(_changes == 1 && strcmp(_changed_path, "eeprom") == 0);
  CHECK(_get("/eeprom", _back, sizeof(_back)) == EEPROM_SIZE && memcmp(_back, _img, EEPROM_SIZE) == 0);

  // shorter (older layout): the rest erased
  for (int i = 0; i < 1000; i++) _img[i] = i ^ 0x5A;
  CHECK(_put("eeprom", _img, 1000, _crc(1000)) == 0 && _changes == 2);
  CHECK(eeprom_read(999) == (uint8_t)(999 ^ 0x5A) && eeprom_read(1000) == 0xFF);
  CHECK(eeprom_read(EEPROM_SIZE - 1) == 0xFF);

  // bad CRC, too short, too large, abandoned: no change
  CHECK(_put("eeprom", _img, 500, 1234) == -84);
  CHECK(_open("eeprom", 500, _crc(500)) == 0 && _req(FILE_SERVER_CLOSE, 0, 0) == -84);
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == -9);
  CHECK(_open("eeprom", EEPROM_SIZE + 1, _crc(EEPROM_SIZE + 1)) == -27);
  CHECK(_open("eeprom", 500, _crc(500)) == 0);
  CHECK(_req_path(FILE_SERVER_STAT, "eeprom") == 0);  // a READ or STAT meanwhile
  CHECK(_open("eeprom", 500, _crc(500)) == 0);       // opened again: from 0
  CHECK(_changes == 2 && eeprom_read(0) == 0x5A && eeprom_read(1000) == 0xFF);
  _put32(w, 0);
  CHECK(_req(FILE_SERVER_CLOSE, 0, 0) == -84);
  CHECK(_req(FILE_SERVER_WRITE, w, 8) == -9);

  // other paths: no room
  CHECK(_put("ppm.txt", _img, 10, _crc(10)) == -28);
  CHECK(_req_path(FILE_SERVER_MKDIR, "tables") == -28);
  char ren[] = "eeprom\0eeprom.bak";
  CHECK(_req(FILE_SERVER_RENAME, ren, sizeof(ren) - 1) == -28);
  CHECK(_req_path(FILE_SERVER_REMOVE, "ppm.txt") == -2);

  // removed: erased
  CHECK(_req_path(FILE_SERVER_REMOVE, "/eeprom") == 0 && _changes == 3);
  erased = 1;
  for (int i = 0; i < EEPROM_SIZE; i++) erased &= eeprom_read(i) == 0xFF;
  CHECK(erased);
}

static void test_retry_log()
{
  // a lost reply: the same seq gets it again, the request not run again
  for (int i = 0; i < 64; i++) _img[i] = 0xA0 + i;
  CHECK(_put("eeprom", _img, 64, _crc(64)) == 0);
  int changes = _changes;
  uint8_t again[FILE_SERVER_REPLY_MAX];
  CHECK(file_server_request(_last_req, _last_req_len, again) == _reply_len);
  CHECK(memcmp(again, _reply, _reply_len) == 0 && _changes == changes);
  // bad CRC: no reply
  uint8_t bad[6] = {++_seq, FILE_SERVER_INFO, 1, 2, 3, 4};
  CHECK(file_server_request(bad, 6, _reply) == 0);

  uint8_t on = 1;
  CHECK(_req(FILE_SERVER_LOG, &on, 1) == 0 && (log_on & LOG_HOST));
  on = 0;
  CHECK(_req(FILE_SERVER_LOG, &on, 1) == 0 && !(log_on & LOG_HOST));
  CHECK(_req(FILE_SERVER_LOG, 0, 0) == -22);
}

int main()
{
  log_init(0);
  eeprom_init(0);
  file_server_init(0, _changed);
  test_list_stat();
  test_read_write();
  test_retry_log();
  return test_done();
}
