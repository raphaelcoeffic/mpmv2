// XJT-Lite bring-up: its I2C EEPROM (DIO9 SDA, DIO10 SCL, 0x51: a 256 byte
// M24C02-W, "402WT"), bay link as xjtl_test.c, results on S.Port:
//
//   p       I2C scan on DIO9/10, both ways: addresses that acknowledge
//   1 / 2   read from address 0 with 1 / 2 address bytes, 8 KB in hex lines
//           (the content repeats every 256 bytes)
//   w / v   eeprom.c: a pattern written to the FrSky clone data and
//           flushed / read back from the chip and checked (the next "w":
//           another pattern)

#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>

#include <stdarg.h>
#include <stdio.h>

#include "board.h"
#include "eeprom.h"
#include "timer.h"
#include "mpm/upstream/mpm_compat.h"

#define BAUDRATE 100000

static void _uart_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }
  UARTDisable(UART0_BASE);
  UARTIntDisable(UART0_BASE, 0xFFF);
  UARTIntClear(UART0_BASE, 0xFFF);
  IOCPortConfigureSet(MODULE_RX, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT | IOC_IOMODE_INV);
  IOCPortConfigureSet(SPORT_TX, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);
  UARTConfigSetExpClk(UART0_BASE, 48000000, BAUDRATE,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN);
  UARTFIFOEnable(UART0_BASE);
  UARTEnable(UART0_BASE);
}

static void _print(const char* fmt, ...)
{
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n++] = '\n';

  GPIO_clearDio(SPORT_TX_EN_N);
  for (int i = 0; i < n; i++) UARTCharPut(UART0_BASE, buf[i]);
  while (UARTBusy(UART0_BASE)) {
  }
  GPIO_setDio(SPORT_TX_EN_N);
}

static void _in(uint8_t dio, uint32_t pull)
{
  GPIO_setOutputEnableDio(dio, GPIO_OUTPUT_DISABLE);
  IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_STD_INPUT | pull);
}

// DIO9/10 released, as the stock app leaves them
static void _release()
{
  _in(IOID_9, IOC_NO_IOPULL);
  _in(IOID_10, IOC_NO_IOPULL);
}

//
// I2C, about 50 kHz: open drain by output enable
//

static uint8_t _sda, _scl;

static void _line(uint8_t dio, uint8_t level)
{
  GPIO_setOutputEnableDio(dio, level ? GPIO_OUTPUT_DISABLE : GPIO_OUTPUT_ENABLE);
  delay_us(10);
}

static void _i2c_start()
{
  _line(_sda, 1);
  _line(_scl, 1);
  _line(_sda, 0);
  _line(_scl, 0);
}

static void _i2c_stop()
{
  _line(_sda, 0);
  _line(_scl, 1);
  _line(_sda, 1);
}

// True if acknowledged
static bool _i2c_byte(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++, v <<= 1) {
    _line(_sda, v >> 7);
    _line(_scl, 1);
    _line(_scl, 0);
  }
  _line(_sda, 1);
  _line(_scl, 1);
  bool ack = !GPIO_readDio(_sda);
  _line(_scl, 0);
  return ack;
}

static uint32_t _i2c_probe(uint8_t sda, uint8_t scl)
{
  _release();
  _sda = sda, _scl = scl;
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t dio = i ? scl : sda;
    IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_STD_INPUT | IOC_IOPULL_UP);
    GPIO_clearDio(dio);
    _line(dio, 1);
  }
  if (!GPIO_readDio(sda) || !GPIO_readDio(scl)) {
    _print("i2c: sda %u scl %u: a line stays low", sda, scl);
    _release();
    return 0;
  }
  uint32_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    _i2c_start();
    bool ack = _i2c_byte(addr << 1);
    _i2c_stop();
    if (ack) {
      _print("i2c: sda %u scl %u: address 0x%02x", sda, scl, addr);
      found++;
    }
  }
  _release();
  return found;
}

// The EEPROM: sequential read from address 0, 1 or 2 address bytes
#define EE_ADDR 0x51
static void _ee_dump(uint8_t addr_bytes)
{
  _release();
  _sda = IOID_9, _scl = IOID_10;
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t dio = i ? IOID_10 : IOID_9;
    IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_STD_INPUT | IOC_IOPULL_UP);
    GPIO_clearDio(dio);
    _line(dio, 1);
  }
  _i2c_start();
  bool ok = _i2c_byte(EE_ADDR << 1);
  for (uint8_t i = 0; i < addr_bytes; i++) ok = _i2c_byte(0) && ok;
  _i2c_start();
  ok = _i2c_byte(EE_ADDR << 1 | 1) && ok;
  if (!ok) _print("dump: no ack");
  char line[80];
  for (uint16_t a = 0; a < 8192; a += 32) {
    int n = snprintf(line, sizeof(line), "%04x ", a);
    for (uint8_t i = 0; i < 32; i++) {
      uint8_t v = 0;
      _line(_sda, 1);
      for (uint8_t b = 0; b < 8; b++) {
        _line(_scl, 1);
        v = v << 1 | GPIO_readDio(_sda);
        _line(_scl, 0);
      }
      bool last = a == 8192 - 32 && i == 31;
      _line(_sda, last);  // ack, nack after the last byte
      _line(_scl, 1);
      _line(_scl, 0);
      n += snprintf(line + n, sizeof(line) - n, "%02x", v);
    }
    _print("%s", line);
  }
  _i2c_stop();
  _release();
}

// eeprom.c's waits: no protocol here
void mpm_poll() {}
int32_t mpm_time_left() { return INT32_MAX; }

static uint8_t _seed;
#define CLONE_START FRSKYD_CLONE_EEPROM_OFFSET
#define CLONE_LEN (3 * 51)

static uint8_t _pattern(uint16_t i) { return (uint8_t)(i * 7 + _seed); }

static void _ee_write()
{
  _seed += 0x35;
  for (uint16_t i = 0; i < CLONE_LEN; i++) eeprom_write(CLONE_START + i, _pattern(i));
  uint32_t t = micros();
  int err = eeprom_flush();
  _print("write: seed %02x, flush %d in %lu us", _seed, err, micros() - t);
}

static void _ee_verify()
{
  eeprom_init(0);
  uint16_t bad = 0;
  for (uint16_t i = 0; i < CLONE_LEN; i++)
    if (eeprom_read(CLONE_START + i) != _pattern(i)) bad++;
  _print("verify: seed %02x, %u/%u bad, offset 0: %02x, 770: %02x, 924: %02x", _seed, bad,
         CLONE_LEN, eeprom_read(0), eeprom_read(770), eeprom_read(924));
}

static void _probe()
{
  uint32_t n = _i2c_probe(IOID_9, IOID_10);
  n += _i2c_probe(IOID_10, IOID_9);
  _print("probe: %lu found", n);
}

int main(void)
{
  board_init();
  timer_init();
  _uart_init();
  _release();
  eeprom_init(0);
  _print("xjtl-eeprom");
  _probe();

  for (;;) {
    while (UARTCharsAvail(UART0_BASE)) {
      int32_t c = UARTCharGetNonBlocking(UART0_BASE);
      if (c == 'p') _probe();
      if (c == '1' || c == '2') _ee_dump(c - '0');
      if (c == 'w') _ee_write();
      if (c == 'v') _ee_verify();
    }
  }
}
