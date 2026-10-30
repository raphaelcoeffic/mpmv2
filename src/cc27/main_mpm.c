// CC2755P10 LaunchPad: the MULTI-Module core (src/main.c on the MPM v2), the
// radio's link on UART0 (cc27/mpm_port.c), the EEPROM in RAM
#include "board.h"
#include "eeprom.h"
#include "log.h"
#include "mpm/mpm.h"
#include "timer.h"

int main(void)
{
  board_init();
  board_hfxt_wait(10000);
  timer_init();
  log_init(millis);
  eeprom_init(0);
  mpm_init();
  mpm_run();
}
