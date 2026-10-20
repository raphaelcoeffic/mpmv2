#include "settings.h"
#include "file_system.h"
#include "mpm/mpm_settings.h"
#include "debug.h"
#include "log.h"

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

// Applies a settings text (empty: the defaults), its number of errors
typedef int (*apply_t)(const char* text, uint32_t len, char* err, uint32_t err_size);

static int _apply_ppm(const char* text, uint32_t len, char* err, uint32_t err_size)
{
  mpm_ppm_settings_t s;
  mpm_ppm_defaults(&s);
  int errors = mpm_ppm_parse(text, len, &s, err, err_size);
  mpm_ppm = s;
  debugln("ppm.txt: protocol %d sub %d rx_num %d option %d, %d errors", s.protocol,
          s.sub_protocol, s.rx_num, s.option, errors);
  return errors;
}

static const char* _debug_setting(void* ctx, mpm_tok_t key, mpm_tok_t val, uint32_t line)
{
  bool* usb = ctx;
  if (!mpm_tok_equal(key, "usb")) return MPM_SETTINGS_UNKNOWN_KEY;
  if (!mpm_tok_bool(val, usb)) return "usb: on or off";
  return 0;
}

static int _apply_debug(const char* text, uint32_t len, char* err, uint32_t err_size)
{
  mpm_settings_err_t e = {err, err_size, 0, 0};
  if (err_size) err[0] = 0;
  bool usb = false;
  mpm_settings_lines(text, len, _debug_setting, &usb, &e);
  log_enable(LOG_FILE, usb);
  debugln("debug.txt: usb %s, %d errors", usb ? "on" : "off", e.count);
  return e.count;
}

static const struct {
  const char* path;
  const char* err_path;
  apply_t apply;
} _files[] = {
    {SETTINGS_PPM, SETTINGS_PPM_ERR, _apply_ppm},
    {SETTINGS_DEBUG, SETTINGS_DEBUG_ERR, _apply_debug},
};

static void _load(uint32_t i)
{
  _err[0] = 0;
  lfs_ssize_t n = _read(_files[i].path);
  if (n < 0) n = 0;
  int errors = _files[i].apply(_text, n, _err, sizeof(_err));
  if (n == sizeof(_text)) {
    const char* msg = "read up to " _STR(SETTINGS_MAX) " bytes only\n";
    uint32_t len = strlen(_err);
    if (len + strlen(msg) < sizeof(_err)) strcpy(_err + len, msg);
    errors++;
  }

  struct lfs_info info;
  if (errors) {
    _write(_files[i].err_path, _err, strlen(_err));
  } else if (lfs_stat(_lfs, _files[i].err_path, &info) == 0) {
    lfs_remove(_lfs, _files[i].err_path);
  }
}

void settings_init(lfs_t* lfs)
{
  _lfs = lfs;
  if (!_lfs) return;
  for (uint32_t i = 0; i < sizeof(_files) / sizeof(_files[0]); i++) _load(i);
}

void settings_file_changed(const char* path)
{
  if (!_lfs) return;
  while (*path == '/') path++;
  for (uint32_t i = 0; i < sizeof(_files) / sizeof(_files[0]); i++)
    if (strcmp(path, _files[i].path) == 0) _load(i);
}
