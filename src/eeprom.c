#include "eeprom.h"
#include "file_system.h"
#include "debug.h"

#include <string.h>

static lfs_t* _lfs;
static uint8_t _data[EEPROM_SIZE];
static uint8_t _flush_data[EEPROM_SIZE];
static bool _init;
static bool _dirty;

static uint8_t _file_buffer[FILE_SYSTEM_CACHE_SIZE];
static const struct lfs_file_config _file_cfg = {.buffer = _file_buffer};

void eeprom_init(lfs_t* lfs)
{
  memset(_data, 0xFF, sizeof(_data));
  _init = true;
  _dirty = false;
  _lfs = lfs;
  if (!_lfs) return;

  lfs_file_t file;
  int err = lfs_file_opencfg(_lfs, &file, EEPROM_FILE, LFS_O_RDONLY, &_file_cfg);
  if (err == LFS_ERR_NOENT) return;
  if (err == LFS_ERR_OK) {
    // a shorter file (older layout): the rest stays erased
    lfs_ssize_t n = lfs_file_read(_lfs, &file, _data, sizeof(_data));
    err = lfs_file_close(_lfs, &file);
    if (n < 0) err = n;
  }
  if (err != LFS_ERR_OK) {
    debugln("error: eeprom read failed (%d), RAM only", err);
    memset(_data, 0xFF, sizeof(_data));
    _lfs = 0;
  }
}

uint8_t eeprom_read(uint16_t address)
{
  if (!_init || address >= EEPROM_SIZE) return 0xFF;
  return _data[address];
}

void eeprom_write(uint16_t address, uint8_t value)
{
  if (!_init) eeprom_init(0);
  if (address >= EEPROM_SIZE || _data[address] == value) return;
  _data[address] = value;
  _dirty = true;
}

bool eeprom_dirty() { return _dirty; }

int eeprom_flush()
{
  if (!_dirty) return LFS_ERR_OK;
  _dirty = false;
  if (!_lfs) return LFS_ERR_OK;

  memcpy(_flush_data, _data, sizeof(_flush_data));
  lfs_file_t file;
  int err = lfs_file_opencfg(_lfs, &file, EEPROM_FILE, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                             &_file_cfg);
  if (err == LFS_ERR_OK) {
    lfs_ssize_t n = lfs_file_write(_lfs, &file, _flush_data, sizeof(_flush_data));
    err = lfs_file_close(_lfs, &file);
    if (n < 0) err = n;
  }
  if (err != LFS_ERR_OK) {
    debugln("error: eeprom write failed (%d), RAM only", err);
    _lfs = 0;
  }
  return err;
}
