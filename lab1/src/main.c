#include "uart.h"

int main(void)
{
    uart_puts("Hello World!1\n");
    while (1) {
        uart_putc(uart_getc());
    }

    return 0;
}