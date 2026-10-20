#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "bench.h"
#include "board.h"
#include "eeprom.h"
#include "file_server.h"
#include "file_system.h"
#include "mpm/mpm.h"
#include "nor_flash.h"
#include "serial.h"
#include "shell.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

#include "debug.h"

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

static bool detect_button()
{
  uint32_t pin = BUTTON;
  IOCPinTypeGpioInput(pin);
  IOCIOPortPullSet(pin, IOC_IOPULL_UP);
  delay_us(100);

  uint32_t start = millis();
  while ((millis() - start) < 10 && !GPIO_readDio(pin)) {}

  return millis() - start >= 10 ? true : false;
}

static void leds_init()
{
#if defined(LED_GREEN) && defined(LED_RED)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#endif

#if defined(LED_SPI) && defined(LED_DIN)
  led_rgb_init(LED_SPI, LED_DIN);
  led_rgb_set_color(LED_COLOR(0x00, 0x00, 0x60));
#endif
}

int main(void)
{
  board_init();
  timer_init();
  leds_init();

  debugln("## Boot completed ##");

#if defined(BENCH)
  bench_loop();
#endif

  bool button_pressed = detect_button();
  if (button_pressed) {
    debugln("button pressed detected");
    shell_loop();
  }

  // The protocols' EEPROM (CYRF ID, RX ids) in the external flash. RAM only
  // without it
  int err = file_system_init(&_lfs);
  if (err != 0) {
    debugln("error: file system init failed (%d)", err);
  } else {
    debugln("file system mounted");
  }
  eeprom_init(err == 0 ? &_lfs : 0);
  // Files from the host over USB (tools/mpm_fs.py)
  file_server_init(err == 0 ? &_lfs : 0);

  mpm_init();
  // Flash work from mpm_port_idle() (EEPROM, files): the protocol keeps
  // running
  nor_flash_set_yield(_flash_yield);
  mpm_run();
}
