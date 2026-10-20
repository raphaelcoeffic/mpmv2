#include "nor_flash.h"
#include "debug.h"
#include "spi.h"

#define FLASH_CMD_READ_JEDEC_ID 0x9f
#define FLASH_CMD_READ_SFDP     0x5a

#define FLASH_CMD_WRITE         0x02
#define FLASH_CMD_READ          0x03

#define FLASH_CMD_STATUS        0x05
#define FLASH_CMD_WRITE_ENABLE  0x06
#define FLASH_CMD_WRITE_STATUS  0x01
#define FLASH_CMD_READ_CONFIG   0x15
#define FLASH_CMD_READ_SECURITY 0x2b

#define FLASH_CMD_ERASE_4KB     0x20
#define FLASH_CMD_ERASE_32KB    0x52
#define FLASH_CMD_CHIP_ERASE    0xc7

#define FLASH_SECTOR_SIZE NOR_FLASH_SECTOR_SIZE
#define FLASH_SECTOR_MASK (FLASH_SECTOR_SIZE - 1)

#define FLASH_BLOCK_SIZE 32768
#define FLASH_BLOCK_MASK (FLASH_BLOCK_SIZE - 1)

#define FLASH_PAGE_SIZE NOR_FLASH_PAGE_SIZE
#define FLASH_PAGE_MASK (FLASH_PAGE_SIZE - 1)

// Security register: last program / erase failed
#define FLASH_SECURITY_P_FAIL (1 << 5)
#define FLASH_SECURITY_E_FAIL (1 << 6)

// Macronix MX25R (MPM v2, LaunchPad): JEDEC manufacturer and memory type.
// Configuration register 2 bit 1: high performance mode (erase 40 ms instead
// of 58, page program 0.85 ms instead of 3.2), volatile
#define MX25R_MANUFACTURER 0xc2
#define MX25R_MEMORY_TYPE  0x28
#define MX25R_CR2_HIGH_PERF (1 << 1)

#define FLASH_DMA_THRESHOLD 8

typedef struct {
  uint8_t manufacturer;
  uint8_t memory_type;
  uint8_t density;
} nor_flash_id_t;

typedef struct {
  nor_flash_id_t id;
  uint32_t log2size;
} nor_flash_descriptor_t;

typedef struct {
  spi_t spi;
  nor_flash_descriptor_t desc;
  void (*yield)();
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

static inline void yield()
{
  if (_flash_state.yield) _flash_state.yield();
}

static void wait_for_not_busy()
{
  uint8_t status;
  for (;;) {
    do_cmd(FLASH_CMD_STATUS, 0, &status, 1);
    if (!(status & 0x01)) break;
    yield();
  }
}

static uint8_t read_security()
{
  uint8_t security;
  do_cmd(FLASH_CMD_READ_SECURITY, 0, &security, 1);
  return security;
}

static void write_enable()
{
  do_cmd(FLASH_CMD_WRITE_ENABLE, 0, 0, 0);
}

static void read_id(nor_flash_id_t* id)
{
  uint8_t buf[3];
  do_cmd(FLASH_CMD_READ_JEDEC_ID, 0, buf, 3);

  id->manufacturer = buf[0];
  id->memory_type = buf[1];
  id->density = buf[2];
}

static void mx25r_high_performance()
{
  uint8_t regs[3];
  do_cmd(FLASH_CMD_STATUS, 0, regs, 1);
  do_cmd(FLASH_CMD_READ_CONFIG, 0, regs + 1, 2);
  if (regs[2] & MX25R_CR2_HIGH_PERF) return;

  regs[2] |= MX25R_CR2_HIGH_PERF;
  write_enable();
  do_cmd(FLASH_CMD_WRITE_STATUS, regs, 0, 3);
  wait_for_not_busy();
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
  int err = read_sfdp();
  if (err) return err;

  const nor_flash_id_t* id = &_flash_state.desc.id;
  debugln("[NOR flash]: JEDEC ID %02x %02x %02x", id->manufacturer, id->memory_type, id->density);
  if (id->manufacturer == MX25R_MANUFACTURER && id->memory_type == MX25R_MEMORY_TYPE) {
    mx25r_high_performance();
  }
  return 0;
}

void nor_flash_set_yield(void (*yield)())
{
  _flash_state.yield = yield;
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

  yield();
  return len;
}

int nor_flash_write(uint32_t address, const uint8_t* data, uint32_t len)
{
  wait_for_not_busy();

  while (len > 0) {
    // a page program wraps within the page
    uint32_t n = FLASH_PAGE_SIZE - (address & FLASH_PAGE_MASK);
    if (n > len) n = len;

    write_enable();
    flash_select();
    put_cmd_addr(FLASH_CMD_WRITE, address);
    flash_write(data, n);
    flash_unselect();

    wait_for_not_busy();
    if (read_security() & FLASH_SECURITY_P_FAIL) return -1;

    address += n;
    data += n;
    len -= n;
  }
  return 0;
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
  return read_security() & FLASH_SECURITY_E_FAIL ? -1 : 0;
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
