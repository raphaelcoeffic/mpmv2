#include "shell.h"

#include "flash_driver.h"
#include "ihex.h"
#include "serial.h"
#include "uart.h"

#include <string.h>

// Commands
#define SERIAL_TEST_CMD 0xAA
#define DUMP_FLASH "dump_flash"
#define LOAD_FLASH "load_flash"

static bool command_chr_equal(char cmd)
{
  return rx_buf[0] == cmd;
}

static bool command_equal(const char* cmd)
{
  return strncmp(cmd, (const char*)rx_buf, rx_len) == 0;
}

static void ihex_flush_cb(char* buffer, unsigned len)
{
  serial_write_dma(buffer, len, true);
}

void shell_loop()
{
  serial_init(SHELL_BAUDRATE, SHELL_SER_MODE);
  
  while (true) {
    if (serial_frame_received) {
      if (rx_len > 0) {
        // Echo RX buffer content
        // serial_write_dma(rx_buf, rx_len);

        if (command_chr_equal(SERIAL_TEST_CMD)) {
          // reply with received len
          uint8_t data = (uint8_t)rx_len;
          serial_write_dma(&data, 1, true);
          goto reset_frame;
        }

        if (command_equal(DUMP_FLASH)) {
          if (flash_init() == 0) {
            ihex_dump_flash(ihex_flush_cb);
          } else {
            serial_print_dma("error initialising flash\n");
          }
          goto reset_frame;
        }

        if (command_equal(LOAD_FLASH)) {
          // TODO
          goto reset_frame;
        }
      }

    reset_frame:
      // Reset RX buffer
      uart_reset_rx_len(UART0);
      serial_frame_received = false;
    }
  }
}

