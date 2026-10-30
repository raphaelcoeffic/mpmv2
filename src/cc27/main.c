// CC2755P10 LaunchPad radio bench (src/main.c under BENCH on the CC2652):
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
