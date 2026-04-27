#ifndef __UART_H__
#define __UART_H__

void uart_init(void);
void uart_set_base(unsigned long base);
void uart_puts(const char *str);
void uart_putc(unsigned char c);
int uart_getc(void);
int uart_getc_raw(void);

/* Top half: called from trap dispatcher with the claimed IRQ id.
 * Drains hardware FIFOs into ring buffers, then enqueues a bottom-
 * half task that will eventually call plic_complete(irq) to unmask
 * the source. The trap dispatcher must NOT call plic_complete()
 * itself for this irq — it is deferred to uart_bottom_half(). */
void uart_top_half(unsigned int irq);

/* Bottom half: completes the PLIC source so the line is unmasked
 * only after the higher-level processing is done. Registered via
 * add_task() from uart_top_half(); the @arg carries the IRQ id. */
void uart_bottom_half(void *arg);

/* Enable IRQ-driven path (after PLIC is online). Before this is called,
 * uart_putc/uart_getc fall back to the blocking poll path so that early
 * boot banners and pre-PLIC input still work. */
void uart_enable_irq_mode(void);

#endif // __UART_H__
