// The MULTI-Module firmware: the board, the scheduler's timer, the log, the
// protocols' EEPROM, then the MPM core (mpm/mpm.h) on the board's
// mpm_port.c. BOARD_FILE_SYSTEM (board.h): littlefs on the external NOR
// flash for the EEPROM file, the settings files and the file server;
// without it the EEPROM is the board's eeprom.c
#include "board.h"
#include "eeprom.h"
#include "mpm/mpm.h"
#include "timer.h"

#include "debug.h"
#include "log.h"

#if defined(BOARD_FILE_SYSTEM)
  #include "file_server.h"
  #include "file_system.h"
  #include "nor_flash.h"
  #include "settings.h"

static lfs_t _lfs;

// Flash waits and reads: the callbacks and the input go on. The next slice
// of flash work (a 256 byte read and its processing by littlefs: up to
// 0.65 ms) only starts if it ends before the next callback; protocols that
// never leave that much: after 20 ms anyway
  #define FLASH_SLICE_US    1000
  #define FLASH_WAIT_MAX_US 20000

static void _flash_yield()
{
  uint32_t start = micros();
  do {
    mpm_poll();
  } while (mpm_time_left() < FLASH_SLICE_US && micros() - start < FLASH_WAIT_MAX_US);
}

static void _storage_init()
{
  // The protocols' EEPROM (CYRF ID, RX ids) in the external flash. RAM only
  // without it
  int err = file_system_init(&_lfs);
  if (err != 0) {
    debugln("error: file system init failed (%d)", err);
  } else {
    debugln("file system mounted");
  }
  eeprom_init(err == 0 ? &_lfs : 0);
  // ppm.txt, debug.txt
  settings_init(err == 0 ? &_lfs : 0);
  // Files from the host over USB (tools/mpm_fs.py), settings applied at once
  file_server_init(err == 0 ? &_lfs : 0, settings_file_changed);
}
#endif

int main(void)
{
  board_init();
  timer_init();
  board_app_init();
  // Debug lines kept from here (log.h), sent as debug.txt or a host says
  log_init(millis);

  debugln("## Boot completed ##");

#if defined(BOARD_FILE_SYSTEM)
  _storage_init();
#else
  eeprom_init(0);
#endif

  mpm_init();
#if defined(BOARD_FILE_SYSTEM)
  // Flash work from mpm_port_idle() (EEPROM, files): the protocol keeps
  // running
  nor_flash_set_yield(_flash_yield);
#endif
  mpm_run();
}
