// The boot loader's CC2652 part (bl_port.h): SysTick at 1 ms, UART0 (the
// console) and UART1 on the radio's pins (BL_RADIO_*, board.h), or UART0 as
// the radio's link (BL_RADIO_UART0: the LaunchPad, whose MPM port talks to
// the radio there), flash through the ROM with the VIMS cache off.

#include <driverlib/cpu.h>
#include <driverlib/flash.h>
#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>
#include <driverlib/vims.h>
#include <inc/hw_nvic.h>

#include "board.h"
#include "bl_port.h"

#define BAUDRATE 57600
#define SYS_CLK  48000000

#define UART_DR_ERRORS 0xF00  // OE, BE, PE, FE in the data register

static const struct {
  uint32_t base;
  bool radio;
} _ports[] = {
#if defined(BL_RADIO_UART0)
    {UART0_BASE, true},
#else
    {UART0_BASE, false},
#endif
#if defined(BL_RADIO_RX)
    {UART1_BASE, true},
#endif
};
#define N_PORTS (sizeof(_ports) / sizeof(_ports[0]))

// --- time: SysTick at 1 ms, polled ---

static uint32_t _ms;

void bl_init(void)
{
  board_init();
  HWREG(NVIC_ST_RELOAD) = SYS_CLK / 1000 - 1;
  HWREG(NVIC_ST_CURRENT) = 0;
  HWREG(NVIC_ST_CTRL) = NVIC_ST_CTRL_CLK_SRC | NVIC_ST_CTRL_ENABLE;
}

// Ticks are lost while flash is erased: timeouts only get longer
uint32_t bl_millis(void)
{
  if (HWREG(NVIC_ST_CTRL) & NVIC_ST_CTRL_COUNT) _ms++;
  return _ms;
}

void bl_deinit(void)
{
  for (uint32_t i = 0; i < N_PORTS; i++) {
    while (UARTBusy(_ports[i].base)) {}
    UARTDisable(_ports[i].base);
  }
#if defined(BL_RADIO_TX)
  // Back to the board_init() levels: S.Port released
  IOCPinTypeGpioOutput(BL_RADIO_TX);
  GPIO_writeDio(BL_RADIO_TX, 1);
  #if defined(MODULE_TX_INV)
  GPIO_writeDio(MODULE_TX_INV, 0);
  #endif
#endif
  HWREG(NVIC_ST_CTRL) = 0;
}

// --- UARTs ---

uint32_t bl_ports_open(void)
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
#if defined(BL_RADIO_RX)
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART1);
#endif
  PRCMLoadSet();
  while (!PRCMLoadGet()) {}

  IOCPinTypeUart(UART0_BASE, SERIAL_RX_IOD, SERIAL_TX_IOD, IOID_UNUSED,
                 IOID_UNUSED);
#if defined(BL_RADIO_RX)
  // RX only: TX stays as board_init() set it until the radio sends a sync
  IOCPortConfigureSet(BL_RADIO_RX, IOC_PORT_MCU_UART1_RX,
                      IOC_STD_INPUT | (BL_RADIO_INVERTED ? IOC_IOMODE_INV : 0));
#endif

  for (uint32_t i = 0; i < N_PORTS; i++) {
    uint32_t base = _ports[i].base;
    UARTDisable(base);
    UARTConfigSetExpClk(base, SYS_CLK, BAUDRATE,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                            UART_CONFIG_PAR_NONE);
    UARTEnable(base);
  }
  return N_PORTS;
}

bool bl_port_radio(uint32_t port) { return _ports[port].radio; }

int bl_port_read(uint32_t port)
{
  uint32_t base = _ports[port].base;
  if (!UARTCharsAvail(base)) return -1;
  uint32_t v = HWREG(base + UART_O_DR);
  return (v & 0xFF) | (v & UART_DR_ERRORS ? BL_RX_ERROR : 0);
}

void bl_port_write(uint32_t port, uint8_t b) { UARTCharPut(_ports[port].base, b); }

void bl_port_take(uint32_t port)
{
#if defined(BL_RADIO_TX)
  if (_ports[port].base != UART1_BASE) return;
  #if defined(MODULE_TX_INV)
  // U9 inverts with TX1_INV set: set it first, the idle GPIO level then
  // holds S.Port at the UART's idle level before the UART takes the pin over
  GPIO_writeDio(MODULE_TX_INV, BL_RADIO_INVERTED);
  IOCPortConfigureSet(BL_RADIO_TX, IOC_PORT_MCU_UART1_TX, IOC_STD_OUTPUT);
  #else
  IOCPortConfigureSet(BL_RADIO_TX, IOC_PORT_MCU_UART1_TX,
                      IOC_STD_OUTPUT | (BL_RADIO_INVERTED ? IOC_IOMODE_INV : 0));
  #endif
#else
  (void)port;
#endif
}

bool bl_radio_absent(void)
{
#if defined(BL_RADIO_ABSENT_LEVEL)
  // DIN reads the pad after the IOC inversion
  return (GPIO_readDio(BL_RADIO_RX) ^ BL_RADIO_INVERTED) == BL_RADIO_ABSENT_LEVEL;
#else
  return false;
#endif
}

// --- flash ---

static uint32_t _vims;

// The cache doesn't see flash writes: off while writing, back through OFF
// (invalidated, VIMSModeSafeSet())
void bl_flash_open(void)
{
  _vims = VIMSModeGet(VIMS_BASE);
  VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_DISABLED, true);
  VIMSLineBufDisable(VIMS_BASE);
}

void bl_flash_close(void)
{
  VIMSLineBufEnable(VIMS_BASE);
  VIMSModeSafeSet(VIMS_BASE, _vims, true);
}

bool bl_flash_erase(uint32_t sector_addr)
{
  CPUcpsid();
  bool ok = FlashSectorErase(sector_addr) == FAPI_STATUS_SUCCESS;
  CPUcpsie();
  return ok;
}

bool bl_flash_program(const uint8_t* data, uint32_t addr, uint32_t len)
{
  CPUcpsid();
  bool ok = FlashProgram((uint8_t*)data, addr, len) == FAPI_STATUS_SUCCESS;
  CPUcpsie();
  return ok;
}
