#include "file_system.h"
#include "nor_flash.h"
#include "board.h"
#include "debug.h"

#define BLOCK_SIZE  (4 * KILOBYTE)
#define BLOCK_COUNT (FS_SIZE / BLOCK_SIZE)

#define FLASH_BLOCK (32 * KILOBYTE)

#define ADDR(block, off) (FS_OFFSET + block * BLOCK_SIZE + off)

const spi_device_t nor_flash = {
  .frame_format = SPI_POL0_PHA0,
  .data_width = 8,
  .bit_rate = 12000000, // 12 MHz
  .rx = FLASH_MISO,
  .tx = FLASH_MOSI,
  .clk = FLASH_SCLK,
  .cs = FLASH_CS,
};

static int _flash_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
  nor_flash_read(ADDR(block, off), (uint8_t*)buffer, size);
  return LFS_ERR_OK;
}

static int _flash_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size)
{
  nor_flash_write(ADDR(block, off), (const uint8_t*)buffer, size);
  return LFS_ERR_OK;
}

static int _flash_erase(const struct lfs_config *c, lfs_block_t block)
{
  nor_flash_erase(ADDR(block, 0));
  return LFS_ERR_OK;
}

static int _flash_sync(const struct lfs_config *c)
{
  nor_flash_sync();
  return LFS_ERR_OK;
}

static void erase_file_system()
{
  debug("erasing file system");
  for (unsigned i = 0; i < BLOCK_COUNT; i++) {
    nor_flash_erase(ADDR(i, 0));
    debug(".");
  }
  debugln(" [done]");
}

#define READ_SIZE      16
#define PROG_SIZE      16
#define CACHE_SIZE     16
#define LOOKAHEAD_SIZE 16

static uint8_t _read_buffer[CACHE_SIZE];
static uint8_t _prog_buffer[CACHE_SIZE];
static uint8_t _lookahead_buffer[CACHE_SIZE];

static const struct lfs_config _flash_cfg = {
    // block device operations
    .read = _flash_read,
    .prog = _flash_prog,
    .erase = _flash_erase,
    .sync = _flash_sync,

    // block device configuration
    .read_size = READ_SIZE,
    .prog_size = PROG_SIZE,
    .block_size = BLOCK_SIZE,
    .block_count = BLOCK_COUNT,
    .cache_size = CACHE_SIZE,
    .lookahead_size = LOOKAHEAD_SIZE,
    .block_cycles = 500,
    .read_buffer = _read_buffer,
    .prog_buffer = _prog_buffer,
    .lookahead_buffer = _lookahead_buffer,
};

int file_system_init(lfs_t* lfs)
{
  if (nor_flash_init(FLASH_SPI, &nor_flash) != 0) {
    debugln("[NOR flash]: error: init failed");
    return LFS_ERR_IO;
  }

  uint32_t flash_size = nor_flash_size();
  uint32_t fs_size = FS_OFFSET + FS_SIZE;
  if (flash_size < fs_size) {
    debugln("error: file system too big (%d > %d)", fs_size, flash_size);
    return LFS_ERR_IO;
  }

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

  int err = lfs_mount(lfs, &_flash_cfg);
  if (err != LFS_ERR_OK) {
    erase_file_system();

    err = lfs_format(lfs, &_flash_cfg);
    if (err == LFS_ERR_OK) {
      err = lfs_mount(lfs, &_flash_cfg);
    } else {
      debugln("error: formatting failed");
    }
  }
  return err;
}
