#include "board.h"

#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_ckmd.h>
#include <inc/hw_ioc.h>
#include <inc/hw_pmctl.h>
#include <inc/hw_systim.h>
#include <driverlib/clkctl.h>
#include <driverlib/gpio.h>
#include <driverlib/uart.h>

#include <stdarg.h>
#include <stdio.h>

// UART0: 48 MHz peripheral clock (CLKSVT / 2)
#define UART_CLK 48000000

#define IOC_REG(dio) (IOC_BASE + IOC_O_IOC0 + 4 * (dio))
#define IOC_UART0 4  // DIO1 TXD, DIO2 RXD

static void _ioc(uint32_t dio, uint32_t cfg) { HWREG(IOC_REG(dio)) = cfg; }

void board_init(void)
{
  // Pads on the VDDIO segment (DIO0 to DIO7 at least, the UART's among
  // them): off until VDDIOPGIO is set (TI's Power_init() clears it)
  HWREG(PMCTL_BASE + PMCTL_O_AONRSET1) = PMCTL_AONRSET1_VDDIOPGIO_SET;

  // HFXT on, and again after standby (as TI's power driver; the amplitude
  // control as the ROM left it)
  HWREG(CKMD_BASE + CKMD_O_HFXTCTL) |= CKMD_HFXTCTL_EN | CKMD_HFXTCTL_AUTOEN;
  // HFOSC (CLKSVT, the LRFD clock) tracks the HFXT once it runs
  HWREG(CKMD_BASE + CKMD_O_HFTRACKCTL) |= CKMD_HFTRACKCTL_EN_M | CKMD_HFTRACKCTL_REFCLK_HFXT;

  CLKCTLEnable(CLKCTL_BASE, CLKCTL_GPIO);

  _ioc(BOARD_LED_RED, IOC_IOC0_PORTCFG_BASE);
  _ioc(BOARD_LED_GREEN, IOC_IOC0_PORTCFG_BASE);
  GPIOClearDio(BOARD_LED_RED);
  GPIOClearDio(BOARD_LED_GREEN);
  GPIOSetOutputEnableDio(BOARD_LED_RED, GPIO_OUTPUT_ENABLE);
  GPIOSetOutputEnableDio(BOARD_LED_GREEN, GPIO_OUTPUT_ENABLE);

  // Buttons to ground
  _ioc(BOARD_BTN1, IOC_IOC0_PORTCFG_BASE | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
  _ioc(BOARD_BTN2, IOC_IOC0_PORTCFG_BASE | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
}

void board_led(uint32_t dio, bool on) { GPIOWriteDio(dio, on ? 1 : 0); }

void board_led_toggle(uint32_t dio) { GPIOToggleDio(dio); }

bool board_button(uint32_t dio) { return GPIOReadDio(dio) == 0; }

bool board_hfxt_good(void)
{
  return (HWREG(CKMD_BASE + CKMD_O_RIS) & CKMD_RIS_AMPSETTLED) != 0;
}

bool board_hfxt_wait(uint32_t timeout_us)
{
  uint32_t start = board_us();
  while (!board_hfxt_good()) {
    if (board_us() - start > timeout_us) return false;
  }
  return true;
}

uint32_t board_us(void) { return HWREG(SYSTIM_BASE + SYSTIM_O_TIME1U); }

void board_delay_us(uint32_t us)
{
  uint32_t start = board_us();
  while (board_us() - start < us) {
  }
}

void uart_init(uint32_t baudrate)
{
  CLKCTLEnable(CLKCTL_BASE, CLKCTL_UART0);
  _ioc(BOARD_UART_TX, IOC_UART0);
  _ioc(BOARD_UART_RX, IOC_UART0 | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);

  UARTDisable(UART0_BASE);
  UARTConfigSetExpClk(UART0_BASE, UART_CLK, baudrate,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
  UARTEnableFifo(UART0_BASE);
  UARTEnable(UART0_BASE);
}

void uart_putc(uint8_t c) { UARTPutChar(UART0_BASE, c); }

void uart_write(const void *buf, uint32_t len)
{
  const uint8_t *p = buf;
  while (len--) uart_putc(*p++);
}

int uart_getc(void)
{
  if (!UARTCharAvailable(UART0_BASE)) return -1;
  return UARTGetCharNonBlocking(UART0_BASE);
}

// Lines end with CR LF (terminals)
void uart_printf(const char *fmt, ...)
{
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  for (int i = 0; i < n; i++) {
    if (buf[i] == '\n') uart_putc('\r');
    uart_putc(buf[i]);
  }
}
