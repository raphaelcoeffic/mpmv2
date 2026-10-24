#pragma once

#include <stdbool.h>
#include <stdint.h>

// The MPM's EEPROM (Multiprotocol.h layout: CYRF ID, RX ids, clone data): a
// RAM copy, written back by eeprom_flush() outside the protocol callbacks.
// eeprom.c: the file "eeprom" in littlefs, which replaces the file at close
// (a power cut keeps the old or the new content); without a file system RAM
// only, erased (0xFF) at power up. mpm/eeprom_xjt.c: the XJT-Lite's I2C
// EEPROM
#define EEPROM_SIZE 2048
#define EEPROM_FILE "eeprom"

// lfs: a mounted file system, or NULL for RAM only (eeprom.c; unused by
// eeprom_xjt.c)
struct lfs;
void eeprom_init(struct lfs* lfs);

uint8_t eeprom_read(uint16_t address);
void eeprom_write(uint16_t address, uint8_t value);

// Written since the last flush
bool eeprom_dirty();

// Writes a copy of the content taken at the start: the flash waits may run
// callbacks that write again (dirty again). 0 or an error (then RAM only from
// there on)
int eeprom_flush();
