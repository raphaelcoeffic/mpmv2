// The boot loader's CC27xx part (bl_port.h): milliseconds from the system
// timer, UART0 (board_uart_init(): the console, or the radio's link with
// BL_RADIO_UART0, the LaunchPad), flash through the ROM (the driverlib
// calls turn the cache and the line buffer off around each write, which
// flushes them).

#include <driverlib/flash.h>
#include <driverlib/uart.h>
#include <inc/hw_uart.h>

#include "board.h"
#include "bl_port.h"

#define BAUDRATE 57600

#define UART_DR_ERRORS (UART_DR_OE | UART_DR_BE | UART_DR_PE | UART_DR_FE)

// --- time ---

static uint32_t _ms, _ms_us;

void bl_init(void)
{
  board_init();
  _ms_us = board_us();
}

uint32_t bl_millis(void)
{
  uint32_t now = board_us();
  while (now - _ms_us >= 1000) {
    _ms_us += 1000;
    _ms++;
  }
  return _ms;
}

void bl_deinit(void)
{
  while (UARTBusy(UART0_BASE)) {}
  UARTDisable(UART0_BASE);
}

// --- UART0 ---

uint32_t bl_ports_open(void)
{
  board_uart_init(BAUDRATE);
  return 1;
}

bool bl_port_radio(uint32_t port)
{
  (void)port;
#if defined(BL_RADIO_UART0)
  return true;
#else
  return false;
#endif
}

int bl_port_read(uint32_t port)
{
  (void)port;
  if (HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE) return -1;
  uint32_t v = HWREG(UART0_BASE + UART_O_DR);
  return (v & 0xFF) | (v & UART_DR_ERRORS ? BL_RX_ERROR : 0);
}

void bl_port_write(uint32_t port, uint8_t b)
{
  (void)port;
  UARTPutChar(UART0_BASE, b);
}

void bl_port_take(uint32_t port) { (void)port; }

bool bl_radio_absent(void) { return false; }

// --- flash: no interrupts enabled in the boot loader ---

void bl_flash_open(void) {}

void bl_flash_close(void) {}

bool bl_flash_erase(uint32_t sector_addr)
{
  return FlashEraseSector(sector_addr) == FAPI_STATUS_SUCCESS;
}

bool bl_flash_program(const uint8_t* data, uint32_t addr, uint32_t len)
{
  return FlashProgram((uint8_t*)data, addr, len) == FAPI_STATUS_SUCCESS;
}
