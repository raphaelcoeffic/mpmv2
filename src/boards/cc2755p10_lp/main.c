// CC2755P10 LaunchPad radio bench (boards/mpm_v2/main.c under BENCH):
// bench.c's shell on UART0, 921600 baud

#include "bench.h"
#include "board.h"
#include "timer.h"

int main(void)
{
  board_init();
  board_hfxt_wait(10000);
  timer_init();
  bench_loop();
}
