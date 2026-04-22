#ifndef __UART_H__
#define __UART_H__

void uart_init(void);
void uart_set_base(unsigned long base);
void uart_puts(const char *str);
void uart_putc(unsigned char c);
int uart_getc(void);
int uart_getc_raw(void);

/* Called from trap dispatcher when PLIC claims a UART IRQ. */
void uart_handle_interrupt(void);

/* Enable IRQ-driven path (after PLIC is online). Before this is called,
 * uart_putc/uart_getc fall back to the blocking poll path so that early
 * boot banners and pre-PLIC input still work. */
void uart_enable_irq_mode(void);

#endif // __UART_H__
