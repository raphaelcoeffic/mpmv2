#include "file_system.h"
#include "ihex.h"
#include "nor_flash.h"

#include "kk_ihex_write.h"

#define READ_BLOCK 256
#define HEX_BLOCK  (READ_BLOCK/16)
#define FILL_BYTE  0xFF

static ihex_flush_cb_t _flush_cb = 0;

void ihex_flush_buffer(struct ihex_state *ihex, char *buffer, char *eptr)
{
  if(_flush_cb) _flush_cb(buffer, eptr - buffer);
}

void ihex_dump_flash(ihex_flush_cb_t cb)
{
  uint8_t buffer[READ_BLOCK];

  struct ihex_state ihex;
  ihex_init(&ihex);

  _flush_cb = cb;

  uint32_t addr = FS_OFFSET;
  while (addr < FS_SIZE) {
    // read a block
    nor_flash_read(addr, buffer, sizeof(buffer));

    uint32_t block_addr = 0;
    while (block_addr < sizeof(buffer)) {
      // skip filling
      while (block_addr < sizeof(buffer)) {
        uint8_t mask = FILL_BYTE;
        for (unsigned i = 0; i < HEX_BLOCK; i++) {
          mask &= buffer[block_addr + i];
        }
        if (mask != FILL_BYTE) break;
        block_addr += HEX_BLOCK;
      }
      if (block_addr == sizeof(buffer)) break;

      ihex_write_at_address(&ihex, addr + block_addr);
      ihex_write_bytes(&ihex, buffer + block_addr, HEX_BLOCK);
      block_addr += HEX_BLOCK;
    }

    // next block
    addr += sizeof(buffer);
  }

  ihex_end_write(&ihex);
  _flush_cb = 0;
}

