// CC2755P10 LaunchPad bring-up: chip and clock status on UART0 (XDS110 back
// channel, 115200 8N1), characters echoed, the red LED blinking, the green
// one on while a button is pressed.

#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_ckmd.h>
#include <inc/hw_fcfg.h>
#include <inc/hw_pmctl.h>

#include "board.h"

int main(void)
{
  uint32_t t0 = board_us();  // runs on through resets
  board_init();
  board_uart_init(115200);

  bool hfxt = board_hfxt_wait(10000);
  uint32_t t_hfxt = board_us() - t0;
  uart_printf("\nCC27xx bring-up\n");
  uart_printf("RESETSTA 0x%08lx\n", HWREG(PMCTL_BASE + PMCTL_O_RSTSTA));
  uart_printf("HFXT %s after %lu us, HFXTSTAT 0x%08lx, RIS 0x%08lx\n",
              hfxt ? "settled" : "not settled", t_hfxt,
              HWREG(CKMD_BASE + CKMD_O_HFXTSTAT), HWREG(CKMD_BASE + CKMD_O_RIS));
  uart_printf("HFXTINIT 0x%08lx, HFXTTARG 0x%08lx\n",
              HWREG(CKMD_BASE + CKMD_O_HFXTINIT), HWREG(CKMD_BASE + CKMD_O_HFXTTARG));

  uint32_t last = board_us();
  for (;;) {
    int c = uart_getc();
    if (c >= 0) uart_putc(c);

    board_led(BOARD_LED_GREEN, board_button(BOARD_BTN1) || board_button(BOARD_BTN2));
    if (board_us() - last > 500000) {
      last += 500000;
      board_led_toggle(BOARD_LED_RED);
    }
  }
}
