#include "nor_flash.h"
#include "debug.h"
#include "spi.h"

#define FLASH_CMD_READ_ID       0x90
#define FLASH_CMD_READ_JEDEC_ID 0x9f
#define FLASH_CMD_READ_SFDP     0x5a

#define FLASH_CMD_WRITE         0x02
#define FLASH_CMD_READ          0x03

#define FLASH_CMD_STATUS        0x05
#define FLASH_CMD_WRITE_ENABLE  0x06

#define FLASH_CMD_ERASE_4KB     0x20
#define FLASH_CMD_ERASE_32KB    0x52
#define FLASH_CMD_CHIP_ERASE    0xc7

#define FLASH_SECTOR_SIZE 4096
#define FLASH_SECTOR_MASK (FLASH_SECTOR_SIZE - 1)

#define FLASH_BLOCK_SIZE 32768
#define FLASH_BLOCK_MASK (FLASH_BLOCK_SIZE - 1)

#define FLASH_PAGE_SIZE 256
#define FLASH_PAGE_MASK (FLASH_PAGE_SIZE - 1)

#define FLASH_DMA_THRESHOLD 8

typedef struct {
  uint8_t vendor_id;
  uint8_t device_id;
} nor_flash_id_t;

typedef struct {
  nor_flash_id_t id;
  uint32_t log2size;
} nor_flash_descriptor_t;

typedef struct {
  spi_t spi;
  nor_flash_descriptor_t desc;
} nor_flash_state_t;

static nor_flash_state_t _flash_state;

static inline void flash_select() {
  spi_select(_flash_state.spi);
}

static inline void flash_unselect() {
  spi_unselect(_flash_state.spi);
}

static inline void flash_transfer(const uint8_t* tx, uint8_t* rx, uint32_t len) {
  spi_transfer_8(_flash_state.spi, tx, rx, len);
}

static inline void flash_write(const uint8_t* data, uint32_t len) {
  if (len > FLASH_DMA_THRESHOLD) {
    spi_write_dma_8(_flash_state.spi, data, len, true);
  } else {
    spi_write_8(_flash_state.spi, data, len);
  }
}

static inline void flash_read(uint8_t* data, uint32_t len) {
  if (len > FLASH_DMA_THRESHOLD) {
    spi_read_dma_8(_flash_state.spi, data, len, true);
  } else {
    spi_read_8(_flash_state.spi, data, len);
  }
}

static inline void put_cmd_addr(uint8_t cmd, uint32_t addr)
{
  uint8_t raw_cmd[4] = {
    cmd,
    (addr >> 16) & 0xFF,
    (addr >> 8) & 0xFF,
    addr & 0xFF,
  };
  flash_write(raw_cmd, sizeof(raw_cmd));
}

static inline void do_cmd(uint8_t cmd, const uint8_t* tx, uint8_t* rx, uint16_t count)
{
  flash_select();
  flash_write(&cmd, 1);
  if (count > 0) flash_transfer(tx, rx, count);
  flash_unselect();
}

static void wait_for_not_busy()
{
  uint8_t status;
  do {
    do_cmd(FLASH_CMD_STATUS, 0, &status, 1);
  } while (status & 0x01);
}

static void write_enable()
{
  do_cmd(FLASH_CMD_WRITE_ENABLE, 0, 0, 0);
}

static void read_id(nor_flash_id_t* id)
{
  uint8_t buf[4] = {FLASH_CMD_READ_ID, 0, 0, 0};

  flash_select();
  flash_write(buf, 4);
  flash_read(buf, 2);
  flash_unselect();

  id->vendor_id = buf[0];
  id->device_id = buf[1];
}

static void read_sfdp_block(uint32_t addr, uint8_t *rx, uint16_t count)
{
  flash_select();
  put_cmd_addr(FLASH_CMD_READ_SFDP, addr);
  flash_write(0, 1); // dummy cycles
  flash_read(rx, count);
  flash_unselect();
}

static inline uint32_t bytes_to_u32le(const uint8_t *b) {
  return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static int read_sfdp()
{
  // check magic signature
  uint8_t rxbuf[16];
  read_sfdp_block(0, rxbuf, 16);

  if (rxbuf[0] != 'S' || rxbuf[1] != 'F' || rxbuf[2] != 'D' || rxbuf[3] != 'P') {
    debugln("[NOR flash]: error: missing SFDP signature");
    return -1;
  }

  // check JEDEC ID is 0
  if (rxbuf[8] != 0) {
    debugln("[NOR flash]: error: JEDEC ID != 0");
    return -1;
  }

  // read param table
  uint32_t param_table_ptr = bytes_to_u32le(rxbuf + 12) & 0xffffffu;
  read_sfdp_block(param_table_ptr, rxbuf, 8);

  // 1st DWORD
  uint32_t param_table_dword = bytes_to_u32le(rxbuf);

  if ((param_table_dword & 3) != 1) {
    // 4 kilobyte erase is unavailable
    debugln("[NOR flash]: 4KB erase not supported");
    return -1;
  }

  if (((param_table_dword >> 17) & 3) != 0) {
    // 4-byte addressing supported
    debugln("[NOR flash]: 4bytes address mode supported");
    // desc->use4BytesAddress = true;
    // flash_do_cmd(FLASH_CMD_EN4B, 0, 0, 0);
  }

  // 2nd DWORD: flash memory density
  // - MSB set: array >= 2 Gbit, encoded as log2 of number of bits
  // - MSB clear: array < 2 Gbit, encoded as direct bit count
  
  param_table_dword = bytes_to_u32le(rxbuf + 4);
  if (param_table_dword & (1u << 31)) {
    param_table_dword &= ~(1u << 31);
  } else {
    uint32_t ctr = 0;
    param_table_dword += 1;
    while (param_table_dword >>= 1)
      ++ctr;
    param_table_dword = ctr;
  }

  uint32_t log2size = param_table_dword - 3;
  _flash_state.desc.log2size = log2size;

  return 0;
}

int nor_flash_init(spi_t spi, const spi_device_t* dev)
{
  spi_init(spi, dev);
  _flash_state.spi = spi;

  read_id(&_flash_state.desc.id);
  // debugln("[NOR flash]: vendor ID = 0x%X", id.vendor_id);
  // debugln("[NOR flash]: device ID = 0x%X", id.device_id);

  return read_sfdp();
}

uint32_t nor_flash_size()
{
  return 1 << _flash_state.desc.log2size;
}

uint32_t nor_flash_read(uint32_t addr, uint8_t* data, uint32_t len)
{
  wait_for_not_busy();

  flash_select();
  put_cmd_addr(FLASH_CMD_READ, addr);
  flash_read(data, len);
  flash_unselect();

  return len;
}

uint32_t nor_flash_write(uint32_t address, const uint8_t* data, uint32_t len)
{
  wait_for_not_busy();
  write_enable();

  flash_select();
  put_cmd_addr(FLASH_CMD_WRITE, address);
  flash_write((uint8_t*)data, len);
  flash_unselect();
  
  wait_for_not_busy();
  return len;
}

void nor_flash_sync()
{
  uint8_t status;
  do {
    do_cmd(FLASH_CMD_STATUS, 0, &status, 1);
  } while (status & 0x01);
}

int nor_flash_erase(uint32_t address)
{
  if((address & FLASH_SECTOR_MASK) != 0)
    return -1;

  wait_for_not_busy();
  write_enable();

  flash_select();
  put_cmd_addr(FLASH_CMD_ERASE_4KB, address);
  flash_unselect();

  wait_for_not_busy();
  return 0;
}

int nor_flash_erase_block(uint32_t address)
{
  // verify block alignment
  if((address & FLASH_BLOCK_MASK) != 0)
    return -1;

  wait_for_not_busy();
  write_enable();
  
  flash_select();
  put_cmd_addr(FLASH_CMD_ERASE_32KB, address);
  flash_unselect();

  wait_for_not_busy();
  return 0;
}

void nor_flash_erase_all()
{
  wait_for_not_busy();
  write_enable();

  do_cmd(FLASH_CMD_CHIP_ERASE, 0, 0, 0);
  wait_for_not_busy();
}
