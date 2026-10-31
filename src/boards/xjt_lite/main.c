// FrSky XJT-Lite: the MULTI-Module core alone (no file system, no settings
// files): the EEPROM on the board's I2C chip (eeprom.c), the PPM
// defaults of mpm_settings.c
#include "board.h"
#include "eeprom.h"
#include "mpm/mpm.h"
#include "timer.h"

#include "debug.h"
#include "log.h"

int main(void)
{
  board_init();
  timer_init();
  log_init(millis);

  debugln("## Boot completed ##");

  eeprom_init(0);
  mpm_init();
  mpm_run();
}
