#include "file_server.h"
#include "file_system.h"
#include "log.h"

#include <lfs_util.h>
#include <string.h>

static lfs_t* _lfs;
static void (*_changed)(const char* path);

static void _notify(const char* path)
{
  if (_changed) _changed(path);
}

// Last reply, sent again for a retried request
static uint8_t _last[FILE_SERVER_REPLY_MAX];
static uint8_t _last_len;

// Upload in progress
static lfs_file_t _up;
static uint8_t _up_buffer[FILE_SYSTEM_CACHE_SIZE];
static const struct lfs_file_config _up_cfg = {.buffer = _up_buffer};
static bool _up_open;
static char _up_path[FILE_SERVER_PATH_MAX + 1];
static uint32_t _up_size, _up_crc, _up_pos, _up_crc_now;

// File read by READ (during an upload too), kept open for the next READ:
// no path lookup and seek for each piece. Closed by the requests that change
// the file system
static lfs_file_t _rd;
static uint8_t _rd_buffer[FILE_SYSTEM_CACHE_SIZE];
static const struct lfs_file_config _rd_cfg = {.buffer = _rd_buffer};
static bool _rd_open;
static char _rd_path[FILE_SERVER_PATH_MAX + 1];

static void _read_close()
{
  if (!_rd_open) return;
  lfs_file_close(_lfs, &_rd);
  _rd_open = false;
}

static uint32_t _crc32(uint32_t crc, const void* data, uint32_t len)
{
  return ~lfs_crc(~crc, data, len);
}

static uint32_t _get32(const uint8_t* p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void _put32(uint8_t* p, uint32_t v)
{
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

// Path of a payload into a terminated string
static bool _path(char* out, const uint8_t* p, uint32_t len)
{
  if (len == 0 || len > FILE_SERVER_PATH_MAX) return false;
  memcpy(out, p, len);
  out[len] = 0;
  return strlen(out) == len;
}

static void _upload_abort()
{
  if (!_up_open) return;
  lfs_file_close(_lfs, &_up);
  lfs_remove(_lfs, FILE_SERVER_UPLOAD);
  _up_open = false;
}

void file_server_sync() { _read_close(); }

void file_server_init(lfs_t* lfs, void (*changed)(const char* path))
{
  _lfs = lfs;
  _changed = changed;
  _last_len = 0;
  _up_open = false;
  _rd_open = false;
  // a previous upload cut short
  if (_lfs) lfs_remove(_lfs, FILE_SERVER_UPLOAD);
}

// The operations: payload in p (n bytes), reply payload into out (*out_n),
// the status
static int _info(uint8_t* out, uint8_t* out_n)
{
  struct lfs_fsinfo info;
  int err = lfs_fs_stat(_lfs, &info);
  if (err) return err;
  lfs_ssize_t used = lfs_fs_size(_lfs);
  if (used < 0) return used;
  _put32(out, info.block_size);
  _put32(out + 4, info.block_count);
  _put32(out + 8, used);
  *out_n = 12;
  return 0;
}

static int _list(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  char path[FILE_SERVER_PATH_MAX + 1];
  if (n < 3 || !_path(path, p + 2, n - 2)) return LFS_ERR_INVAL;
  uint16_t index = p[0] | p[1] << 8;

  lfs_dir_t dir;
  int err = lfs_dir_open(_lfs, &dir, path);
  if (err) return err;
  // room for the reply's header, status and CRC
  uint32_t room = FILE_SERVER_REPLY_MAX - 3 - 4;
  uint32_t len = 1;
  uint16_t i = 0;
  out[0] = 0;
  struct lfs_info info;
  while ((err = lfs_dir_read(_lfs, &dir, &info)) > 0) {
    if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
    if (i++ < index) continue;
    uint32_t name_len = strlen(info.name);
    if (len + 6 + name_len > room) {
      out[0] = 1;  // more
      break;
    }
    out[len] = info.type == LFS_TYPE_DIR ? 2 : 1;
    _put32(out + len + 1, info.size);
    out[len + 5] = name_len;
    memcpy(out + len + 6, info.name, name_len);
    len += 6 + name_len;
  }
  lfs_dir_close(_lfs, &dir);
  if (err < 0) return err;
  *out_n = len;
  return 0;
}

static int _stat(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  char path[FILE_SERVER_PATH_MAX + 1];
  if (!_path(path, p, n)) return LFS_ERR_INVAL;
  struct lfs_info info;
  int err = lfs_stat(_lfs, path, &info);
  if (err) return err;
  out[0] = info.type == LFS_TYPE_DIR ? 2 : 1;
  _put32(out + 1, info.size);
  *out_n = 5;
  return 0;
}

static int _read(const uint8_t* p, uint32_t n, uint8_t* out, uint8_t* out_n)
{
  char path[FILE_SERVER_PATH_MAX + 1];
  if (n < 6 || !_path(path, p + 5, n - 5)) return LFS_ERR_INVAL;
  uint32_t offset = _get32(p);
  uint8_t len = p[4];
  if (len > FILE_SERVER_DATA_MAX) return LFS_ERR_INVAL;

  if (!_rd_open || strcmp(path, _rd_path) != 0) {
    _read_close();
    int err = lfs_file_opencfg(_lfs, &_rd, path, LFS_O_RDONLY, &_rd_cfg);
    if (err) return err;
    _rd_open = true;
    strcpy(_rd_path, path);
  }
  lfs_soff_t pos = lfs_file_tell(_lfs, &_rd);
  if (pos >= 0 && (uint32_t)pos != offset) pos = lfs_file_seek(_lfs, &_rd, offset, LFS_SEEK_SET);
  lfs_ssize_t got = pos < 0 ? pos : lfs_file_read(_lfs, &_rd, out, len);
  if (got < 0) {
    _read_close();
    return got;
  }
  *out_n = got;
  return 0;
}

static int _open(const uint8_t* p, uint32_t n)
{
  _upload_abort();
  if (n < 9 || !_path(_up_path, p + 8, n - 8)) return LFS_ERR_INVAL;
  int err = lfs_file_opencfg(_lfs, &_up, FILE_SERVER_UPLOAD,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &_up_cfg);
  if (err) return err;
  _up_open = true;
  _up_size = _get32(p);
  _up_crc = _get32(p + 4);
  _up_pos = 0;
  _up_crc_now = 0;
  return 0;
}

static int _write(const uint8_t* p, uint32_t n)
{
  if (!_up_open) return LFS_ERR_BADF;
  if (n < 4) return LFS_ERR_INVAL;
  uint32_t offset = _get32(p);
  uint32_t len = n - 4;
  if (offset != _up_pos || _up_pos + len > _up_size) return LFS_ERR_INVAL;
  lfs_ssize_t done = lfs_file_write(_lfs, &_up, p + 4, len);
  if (done < 0) {
    _upload_abort();
    return done;
  }
  _up_crc_now = _crc32(_up_crc_now, p + 4, len);
  _up_pos += len;
  return 0;
}

static int _close()
{
  if (!_up_open) return LFS_ERR_BADF;
  if (_up_pos != _up_size || _up_crc_now != _up_crc) {
    _upload_abort();
    return LFS_ERR_CORRUPT;
  }
  _up_open = false;
  int err = lfs_file_close(_lfs, &_up);
  if (!err) err = lfs_rename(_lfs, FILE_SERVER_UPLOAD, _up_path);
  if (err) lfs_remove(_lfs, FILE_SERVER_UPLOAD);
  else _notify(_up_path);
  return err;
}

static int _remove(const uint8_t* p, uint32_t n)
{
  char path[FILE_SERVER_PATH_MAX + 1];
  if (!_path(path, p, n)) return LFS_ERR_INVAL;
  int err = lfs_remove(_lfs, path);
  if (!err) _notify(path);
  return err;
}

static int _rename(const uint8_t* p, uint32_t n)
{
  const uint8_t* sep = memchr(p, 0, n);
  if (!sep) return LFS_ERR_INVAL;
  char from[FILE_SERVER_PATH_MAX + 1], to[FILE_SERVER_PATH_MAX + 1];
  uint32_t from_len = sep - p;
  if (!_path(from, p, from_len) || !_path(to, sep + 1, n - from_len - 1)) return LFS_ERR_INVAL;
  int err = lfs_rename(_lfs, from, to);
  if (!err) {
    _notify(from);
    _notify(to);
  }
  return err;
}

static int _mkdir(const uint8_t* p, uint32_t n)
{
  char path[FILE_SERVER_PATH_MAX + 1];
  if (!_path(path, p, n)) return LFS_ERR_INVAL;
  return lfs_mkdir(_lfs, path);
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
  if (_lfs && op != FILE_SERVER_READ && op != FILE_SERVER_STAT && op != FILE_SERVER_LIST &&
      op != FILE_SERVER_INFO && op != FILE_SERVER_LOG) {
    _read_close();
  }
  if (op == FILE_SERVER_LOG) {
    status = n == 1 ? 0 : LFS_ERR_INVAL;
    if (status == 0) log_enable(LOG_HOST, p[0]);
  } else if (!_lfs) {
    status = LFS_ERR_IO;
  } else {
    switch (op) {
      case FILE_SERVER_INFO: status = _info(out, &out_n); break;
      case FILE_SERVER_LIST: status = _list(p, n, out, &out_n); break;
      case FILE_SERVER_STAT: status = _stat(p, n, out, &out_n); break;
      case FILE_SERVER_READ: status = _read(p, n, out, &out_n); break;
      case FILE_SERVER_OPEN: status = _open(p, n); break;
      case FILE_SERVER_WRITE: status = _write(p, n); break;
      case FILE_SERVER_CLOSE: status = _close(); break;
      case FILE_SERVER_REMOVE: status = _remove(p, n); break;
      case FILE_SERVER_RENAME: status = _rename(p, n); break;
      case FILE_SERVER_MKDIR: status = _mkdir(p, n); break;
      default: status = LFS_ERR_INVAL; break;
    }
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
