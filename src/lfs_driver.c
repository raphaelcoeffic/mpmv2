#include "file_system.h"
#include "flash_driver.h"
#include "nor_flash.h"
#include "debug.h"

#define BLOCK_SIZE NOR_FLASH_SECTOR_SIZE

#define ADDR(block, off) ((block) * BLOCK_SIZE + (off))

static int _flash_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
  nor_flash_read(ADDR(block, off), (uint8_t*)buffer, size);
  return LFS_ERR_OK;
}

// A failed program or erase: littlefs moves the data to another block
static int _flash_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size)
{
  if (nor_flash_write(ADDR(block, off), (const uint8_t*)buffer, size) != 0) {
    return LFS_ERR_CORRUPT;
  }
  return LFS_ERR_OK;
}

static int _flash_erase(const struct lfs_config *c, lfs_block_t block)
{
  if (nor_flash_erase(ADDR(block, 0)) != 0) {
    return LFS_ERR_CORRUPT;
  }
  return LFS_ERR_OK;
}

static int _flash_sync(const struct lfs_config *c)
{
  nor_flash_sync();
  return LFS_ERR_OK;
}

// Lookahead: 256 blocks (bits), a window over larger flashes
#define READ_SIZE      16
#define PROG_SIZE      16
#define CACHE_SIZE     FILE_SYSTEM_CACHE_SIZE
#define LOOKAHEAD_SIZE 32

static uint8_t _read_buffer[CACHE_SIZE];
static uint8_t _prog_buffer[CACHE_SIZE];
static uint8_t _lookahead_buffer[LOOKAHEAD_SIZE];

static struct lfs_config _flash_cfg = {
    // block device operations
    .read = _flash_read,
    .prog = _flash_prog,
    .erase = _flash_erase,
    .sync = _flash_sync,

    // block device configuration
    .read_size = READ_SIZE,
    .prog_size = PROG_SIZE,
    .block_size = BLOCK_SIZE,
    .block_count = 0,  // mount: the file system's
    .cache_size = CACHE_SIZE,
    .lookahead_size = LOOKAHEAD_SIZE,
    .block_cycles = 500,
    .read_buffer = _read_buffer,
    .prog_buffer = _prog_buffer,
    .lookahead_buffer = _lookahead_buffer,
};

int file_system_init(lfs_t* lfs)
{
  if (flash_init() != 0) {
    debugln("error: flash init failed");
    return LFS_ERR_IO;
  }

  uint32_t flash_size = nor_flash_size();
  lfs_size_t block_count = flash_size / BLOCK_SIZE;

#if defined(DEBUG)
  char* unit = "B";
  if (flash_size >= MEGABYTE) {
    flash_size /= MEGABYTE;
    unit = "MB";
  } else if (flash_size >= KILOBYTE) {
    flash_size /= KILOBYTE;
    unit = "KB";
  }
  debugln("flash size = %d %s", flash_size, unit);
#endif

  _flash_cfg.block_count = 0;
  int err = lfs_mount(lfs, &_flash_cfg);
  if (err == LFS_ERR_CORRUPT) {
    // no file system (blank or foreign data): only then format, a bus or
    // version error keeps what is there
    debugln("formatting the file system");
    _flash_cfg.block_count = block_count;
    err = lfs_format(lfs, &_flash_cfg);
    if (err == LFS_ERR_OK) {
      err = lfs_mount(lfs, &_flash_cfg);
    } else {
      debugln("error: formatting failed (%d)", err);
    }
  }
  if (err != LFS_ERR_OK) {
    debugln("error: mount failed (%d)", err);
    return err;
  }

  struct lfs_fsinfo info;
  err = lfs_fs_stat(lfs, &info);
  if (err == LFS_ERR_OK && info.block_count < block_count) {
    debugln("growing the file system to %d blocks", block_count);
    err = lfs_fs_grow(lfs, block_count);
  }
  if (err != LFS_ERR_OK) {
    lfs_unmount(lfs);
  }
  return err;
}
