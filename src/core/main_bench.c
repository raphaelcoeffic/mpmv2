// The radio bench (docs/bench.md): bench.c's shell instead of the MPM, on
// the board's bench port (bench_port.h)
#include "bench.h"
#include "board.h"
#include "timer.h"

int main(void)
{
  board_init();
  timer_init();
  board_app_init();
  bench_loop();
}
