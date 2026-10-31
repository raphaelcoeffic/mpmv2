// eeprom.h on the CC2755P10 LaunchPad: RAM only, erased (0xFF) at power up
// (no file system; the FrSky and DSM bind data come again from a bind)
#include "eeprom.h"

#include <string.h>

static uint8_t _data[EEPROM_SIZE];
static bool _init;

void eeprom_init(struct lfs* lfs)
{
  (void)lfs;
  memset(_data, 0xFF, sizeof(_data));
  _init = true;
}

uint8_t eeprom_read(uint16_t address)
{
  if (!_init || address >= EEPROM_SIZE) return 0xFF;
  return _data[address];
}

void eeprom_write(uint16_t address, uint8_t value)
{
  if (!_init) eeprom_init(0);
  if (address < EEPROM_SIZE) _data[address] = value;
}

bool eeprom_dirty() { return false; }

int eeprom_flush() { return 0; }
