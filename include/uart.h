#ifndef __UART_H__
#define __UART_H__

void uart_init(void);
void uart_puts(const char *str);
void uart_putc(unsigned char c);
int uart_getc(void);
int uart_getc_raw(void);

#endif // __UART_H__