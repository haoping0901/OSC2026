#include "uart.h"
#include "shell.h"

int main(void)
{
    uart_puts("Welcome to OPI-RV2!\n");
    shell();

    return 0;
}