#include "settings.h"
#include "file_system.h"
#include "mpm/mpm_settings.h"
#include "debug.h"

#include <string.h>

#define _STR(x) _STR2(x)
#define _STR2(x) #x

static lfs_t* _lfs;

static char _text[SETTINGS_MAX];
static char _err[512];
static uint8_t _file_buffer[FILE_SYSTEM_CACHE_SIZE];
static const struct lfs_file_config _file_cfg = {.buffer = _file_buffer};

// The file's content into _text, its length; -1 if there is none
static lfs_ssize_t _read(const char* path)
{
  lfs_file_t file;
  int err = lfs_file_opencfg(_lfs, &file, path, LFS_O_RDONLY, &_file_cfg);
  if (err) return -1;
  lfs_ssize_t n = lfs_file_read(_lfs, &file, _text, sizeof(_text));
  lfs_file_close(_lfs, &file);
  return n;
}

static void _write(const char* path, const char* text, uint32_t len)
{
  lfs_file_t file;
  int err = lfs_file_opencfg(_lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                             &_file_cfg);
  if (err) return;
  lfs_file_write(_lfs, &file, text, len);
  lfs_file_close(_lfs, &file);
}

static void _load_ppm()
{
  mpm_ppm_settings_t s;
  mpm_ppm_defaults(&s);
  _err[0] = 0;
  int errors = 0;
  lfs_ssize_t n = _read(SETTINGS_PPM);
  if (n >= 0) {
    errors = mpm_ppm_parse(_text, n, &s, _err, sizeof(_err));
    if (n == sizeof(_text)) {
      const char* msg = "read up to " _STR(SETTINGS_MAX) " bytes only\n";
      uint32_t len = strlen(_err);
      if (len + strlen(msg) < sizeof(_err)) strcpy(_err + len, msg);
      errors++;
    }
  }
  mpm_ppm = s;
  debugln("ppm: protocol %d sub %d rx_num %d, %d errors", s.protocol, s.sub_protocol, s.rx_num,
          errors);

  struct lfs_info info;
  if (errors) {
    _write(SETTINGS_PPM_ERR, _err, strlen(_err));
  } else if (lfs_stat(_lfs, SETTINGS_PPM_ERR, &info) == 0) {
    lfs_remove(_lfs, SETTINGS_PPM_ERR);
  }
}

void settings_init(lfs_t* lfs)
{
  _lfs = lfs;
  if (_lfs) _load_ppm();
}

void settings_file_changed(const char* path)
{
  if (!_lfs) return;
  while (*path == '/') path++;
  if (strcmp(path, SETTINGS_PPM) == 0) _load_ppm();
}
