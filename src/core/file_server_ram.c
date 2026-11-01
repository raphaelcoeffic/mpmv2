// file_server.h without a file system: the file "eeprom" only, the EEPROM's
// RAM copy (eeprom.h) read and written through it, as the MPM v2's file
// (tools/mpm_fs.py get / put / eeprom; tools/bench/radio_sim.py --eeprom).
// An upload replaces the copy at CLOSE once its size and CRC-32 check (a
// shorter file: the rest erased), REMOVE erases it. "/" lists the file;
// other paths do not exist, and there is no room for new ones.
#include "file_server.h"
#include "eeprom.h"
#include "log.h"

#include <stdbool.h>
#include <string.h>

// littlefs's error codes (lfs.h), what the host prints
#define ERR_NOENT   -2
#define ERR_IO      -5
#define ERR_BADF    -9
#define ERR_ISDIR   -21
#define ERR_INVAL   -22
#define ERR_FBIG    -27
#define ERR_NOSPC   -28
#define ERR_CORRUPT -84

static void (*_changed)(const char* path);

// Last reply, sent again for a retried request
static uint8_t _last[FILE_SERVER_REPLY_MAX];
static uint8_t _last_len;

// Upload in progress: the new content until CLOSE
static uint8_t _up[EEPROM_SIZE];
static bool _up_open;
static uint32_t _up_size, _up_crc, _up_pos;

// zlib's CRC-32, bit by bit
static uint32_t _crc32(uint32_t crc, const uint8_t* data, uint32_t len)
{
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
  }
  return ~crc;
}

static uint32_t _get32(const uint8_t* p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void _put32(uint8_t* p, uint32_t v)
{
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

// A payload's path, leading slashes off: 0 the root, 1 the EEPROM file,
// -1 anything else
static int _path(const uint8_t* p, uint32_t len)
{
  if (len == 0 || len > FILE_SERVER_PATH_MAX || memchr(p, 0, len)) return -1;
  while (len && *p == '/') p++, len--;
  if (len == 0) return 0;
  return len == strlen(EEPROM_FILE) && memcmp(p, EEPROM_FILE, len) == 0 ? 1 : -1;
}

void file_server_sync() {}

void file_server_init(struct lfs* lfs, void (*changed)(const char* path))
{
  (void)lfs;
  _changed = changed;
  _last_len = 0;
  _up_open = false;
}

static int _list(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  if (n < 3 || _path(p + 2, n - 2) != 0) return n < 3 ? ERR_INVAL : ERR_NOENT;
  uint16_t index = p[0] | p[1] << 8;
  uint32_t name_len = strlen(EEPROM_FILE);
  out[0] = 0;  // no more
  *out_n = 1;
  if (index == 0) {
    out[1] = 1;
    _put32(out + 2, EEPROM_SIZE);
    out[6] = name_len;
    memcpy(out + 7, EEPROM_FILE, name_len);
    *out_n += 6 + name_len;
  }
  return 0;
}

static int _stat(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  int path = _path(p, n);
  if (path < 0) return ERR_NOENT;
  out[0] = path ? 1 : 2;
  _put32(out + 1, path ? EEPROM_SIZE : 0);
  *out_n = 5;
  return 0;
}

static int _read(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  if (n < 6) return ERR_INVAL;
  int path = _path(p + 5, n - 5);
  if (path < 0) return ERR_NOENT;
  if (path == 0) return ERR_ISDIR;
  uint32_t offset = _get32(p);
  uint8_t len = p[4];
  if (len > FILE_SERVER_DATA_MAX) return ERR_INVAL;
  if (offset > EEPROM_SIZE) offset = EEPROM_SIZE;
  if (len > EEPROM_SIZE - offset) len = EEPROM_SIZE - offset;
  for (uint8_t i = 0; i < len; i++) out[i] = eeprom_read(offset + i);
  *out_n = len;
  return 0;
}

static int _open(const uint8_t* p, uint32_t n)
{
  _up_open = false;
  if (n < 9) return ERR_INVAL;
  if (_path(p + 8, n - 8) != 1) return ERR_NOSPC;
  _up_size = _get32(p);
  if (_up_size > EEPROM_SIZE) return ERR_FBIG;
  _up_crc = _get32(p + 4);
  _up_pos = 0;
  _up_open = true;
  return 0;
}

static int _write(const uint8_t* p, uint32_t n)
{
  if (!_up_open) return ERR_BADF;
  if (n < 4) return ERR_INVAL;
  uint32_t offset = _get32(p);
  uint32_t len = n - 4;
  if (offset != _up_pos || _up_pos + len > _up_size) return ERR_INVAL;
  memcpy(_up + _up_pos, p + 4, len);
  _up_pos += len;
  return 0;
}

static int _close()
{
  if (!_up_open) return ERR_BADF;
  _up_open = false;
  if (_up_pos != _up_size || _crc32(0, _up, _up_size) != _up_crc) return ERR_CORRUPT;
  for (uint32_t i = 0; i < EEPROM_SIZE; i++) eeprom_write(i, i < _up_size ? _up[i] : 0xFF);
  if (_changed) _changed(EEPROM_FILE);
  return 0;
}

static int _remove(const uint8_t* p, uint32_t n)
{
  if (_path(p, n) != 1) return ERR_NOENT;
  for (uint32_t i = 0; i < EEPROM_SIZE; i++) eeprom_write(i, 0xFF);
  if (_changed) _changed(EEPROM_FILE);
  return 0;
}

uint8_t file_server_request(const uint8_t* req, uint8_t len, uint8_t* reply)
{
  if (len < 6) return 0;
  uint32_t n = len - 6;  // payload
  if (_get32(req + len - 4) != _crc32(0, req, len - 4)) return 0;
  uint8_t seq = req[0], op = req[1];
  const uint8_t* p = req + 2;

  if (_last_len && _last[0] == seq && _last[1] == op) {
    memcpy(reply, _last, _last_len);
    return _last_len;
  }

  uint8_t* out = reply + 3;
  uint8_t out_n = 0;
  int status;
  switch (op) {
    case FILE_SERVER_INFO: status = ERR_IO; break;  // no file system
    case FILE_SERVER_LIST: status = _list(p, n, out, &out_n); break;
    case FILE_SERVER_STAT: status = _stat(p, n, out, &out_n); break;
    case FILE_SERVER_READ: status = _read(p, n, out, &out_n); break;
    case FILE_SERVER_OPEN: status = _open(p, n); break;
    case FILE_SERVER_WRITE: status = _write(p, n); break;
    case FILE_SERVER_CLOSE: status = _close(); break;
    case FILE_SERVER_REMOVE: status = _remove(p, n); break;
    case FILE_SERVER_LOG:
      status = n == 1 ? 0 : ERR_INVAL;
      if (status == 0) log_enable(LOG_HOST, p[0]);
      break;
    default: status = ERR_NOSPC; break;  // RENAME, MKDIR
  }
  if (status) out_n = 0;

  reply[0] = seq;
  reply[1] = op;
  reply[2] = (uint8_t)(int8_t)status;
  uint32_t reply_len = 3 + out_n;
  _put32(reply + reply_len, _crc32(0, reply, reply_len));
  reply_len += 4;

  memcpy(_last, reply, reply_len);
  _last_len = reply_len;
  return reply_len;
}
