#include "file_system.h"
#include "ihex.h"
#include "nor_flash.h"

#include "kk_ihex_write.h"

static ihex_flush_cb_t _flush_cb = 0;

void ihex_flush_buffer(struct ihex_state *ihex, char *buffer, char *eptr)
{
  if(_flush_cb) _flush_cb(buffer, eptr - buffer);
}

void ihex_dump_flash(ihex_flush_cb_t cb)
{
  unsigned block_size = 128;
  uint8_t buffer[block_size];

  struct ihex_state ihex;
  ihex_init(&ihex);

  _flush_cb = cb;

  uint32_t addr = FS_OFFSET;
  while (addr < FS_SIZE) {
    // read a block
    nor_flash_read(addr, buffer, block_size);

    uint32_t block_addr = 0;
    while (block_addr < block_size) {
      // skip filling
      while (block_addr < block_size) {
        if (((uint32_t*)buffer)[block_addr/4] != 0xFFFFFFFF) break;
        if (((uint32_t*)buffer)[block_addr/4 + 1] != 0xFFFFFFFF) break;
        if (((uint32_t*)buffer)[block_addr/4 + 2] != 0xFFFFFFFF) break;
        if (((uint32_t*)buffer)[block_addr/4 + 3] != 0xFFFFFFFF) break;
        block_addr += 16;
      }
      if (block_addr == block_size) break;

      ihex_write_at_address(&ihex, addr + block_addr);
      ihex_write_bytes(&ihex, buffer + block_addr, 16);
      block_addr += 16;
    }

    // next block
    addr += block_size;
  }

  ihex_end_write(&ihex);
  _flush_cb = 0;
}

