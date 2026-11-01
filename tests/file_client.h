#pragma once

// The host's side of file_server.h for the tests: requests as
// tools/mpm_fs.py sends them, each reply checked (seq, op, CRC-32)

#include <stdint.h>
#include <string.h>

#include "file_server.h"
#include "test.h"

// zlib's CRC-32, bit by bit
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
