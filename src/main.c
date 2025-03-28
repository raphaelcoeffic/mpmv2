#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/gpio.h>
#include <driverlib/uart.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/systick.h>
#include <driverlib/interrupt.h>
#include <driverlib/watchdog.h>

// CC2652R1-LAUNCHXL
#define SERIAL_RX_IOD   IOID_2
#define SERIAL_TX_IOD   IOID_3

#define LED1_IOD IOID_7
#define LED2_IOD IOID_6

#define SERIAL_BAUDRATE 115200

void board_init()
{
    CPUcpsid();

    //
    // Power ON: Periph & Serial
    //
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)
           != PRCM_DOMAIN_POWER_ON);

    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // LEDs
    IOCPinTypeGpioOutput(LED1_IOD);
    IOCPinTypeGpioOutput(LED2_IOD);

    // Serial
    IOCPinTypeUart(UART0_BASE, SERIAL_RX_IOD, SERIAL_TX_IOD, IOID_UNUSED, IOID_UNUSED);

    UARTDisable(UART0_BASE);
    UARTConfigSetExpClk(UART0_BASE, SysCtrlClockGet(), SERIAL_BAUDRATE,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);

    CPUcpsie();
}

void uart_print(const char* str)
{
    while(*str) {
        UARTCharPut(UART0_BASE, *str);
        str++;
    }
}

int main(void)
{
    // Init chip and peripherals
    board_init();

    GPIO_clearDio(LED1_IOD);
    GPIO_clearDio(LED2_IOD);

    while (1) {
        CPUdelay(10000000);
        GPIO_toggleDio(LED1_IOD);
        GPIO_toggleDio(LED2_IOD);
        uart_print("#\n");
    }
}

