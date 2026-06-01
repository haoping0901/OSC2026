#include "uart.h"
#include "riscv.h"
#include "types.h"
#include "task.h"
#include "plic.h"
#include "sched.h"

/* Runtime UART base address – overridden by uart_set_base() after DTB parse */
static volatile unsigned long g_uart_base;

/* Flipped to 1 by uart_enable_irq_mode() once PLIC is wired up; until
 * then uart_putc / uart_getc stay on the polled path so early banners and
 * the first shell prompt work even before interrupts are online. */
static volatile int g_uart_irq_mode;

void uart_set_base(unsigned long base)
{
    if (base)
        g_uart_base = base;
}

#define UART_BASE g_uart_base

#ifndef QEMU
#define UART_RBR    0x00
#define UART_THR    0x00
#define UART_DLL    0x00
#define UART_DLH    0x04
#define UART_IER    0x04
#define UART_FCR    0x08
#define UART_LCR    0x0C
#define UART_MCR    0x10
#define UART_LSR    0x14
#else
#define UART_RBR    0x00
#define UART_THR    0x00
#define UART_IER    0x01
#define UART_FCR    0x02
#define UART_LCR    0x03
#define UART_MCR    0x04
#define UART_LSR    0x05
#endif // !QEMU

/* MCR.OUT2 gates the external IRQ line on 16550/PXA UARTs: without it
 * asserted the IER may be set and events fire in LSR, but no IRQ ever
 * reaches the PLIC. */
#define MCR_OUT2        (1 << 3)

/* IER bits */
#define IER_RAVIE       (1 << 0)    /* Receiver Data Available Interrupt Enable  */
#define IER_TIE         (1 << 1)    /* Transmit Data Request Interrupt Enable    */
#define IER_RLSE        (1 << 2)    /* Receiver Line Status Interrupt Enable     */
#define IER_MIE         (1 << 3)    /* Modem Interrupt Enable                    */
#define IER_RTOIE       (1 << 4)    /* Receiver Time-out Interrupt Enable        */
#define IER_NRZE        (1 << 5)    /* NRZ Coding Enable                         */
#define IER_UUE         (1 << 6)    /* UART Unit Enable                          */
#define IER_DMAE        (1 << 7)    /* DMA Requests Enable                       */

/* FCR bits */
#define FCR_TRFIFOE     (1 << 0)    /* Transmit and Receive FIFO Enable         */
#define FCR_RESETRF     (1 << 1)    /* Reset Receive FIFO                       */
#define FCR_RESETTF     (1 << 2)    /* Reset Transmit FIFO                      */
#define FCR_TIL         (1 << 3)    /* Transmitter Interrupt Level              */
#define FCR_TRAIL       (1 << 4)    /* Trailing Bytes                           */
#define FCR_ITL_1       (0 << 6)    /* Interrupt Trigger Level: 1 byte          */
#define FCR_ITL_8       (1 << 6)    /* Interrupt Trigger Level: 8 bytes         */
#define FCR_ITL_16      (2 << 6)    /* Interrupt Trigger Level: 16 bytes        */
#define FCR_ITL_32      (3 << 6)    /* Interrupt Trigger Level: 32 bytes        */

/* LCR bits */
#define LCR_WLS_8       (0x3 << 0)  /* 8-bit word length                        */
#define LCR_WLS_7       (0x2 << 0)  /* 7-bit word length                        */
#define LCR_STB_1       (0 << 2)    /* 1 stop bit                               */
#define LCR_PEN         (1 << 3)    /* Parity Enable                            */
#define LCR_EPS         (1 << 4)    /* Even Parity Select                       */
#define LCR_STKYP       (1 << 5)    /* Sticky Parity                            */
#define LCR_SB          (1 << 6)    /* Set Break                                */
#define LCR_DLAB        (1 << 7)    /* Divisor Latch Access Bit                 */

/* LSR bits */
#define LSR_DR          (1 << 0)    /* Data Ready                               */
#define LSR_OE          (1 << 1)    /* Overrun Error                            */
#define LSR_PE          (1 << 2)    /* Parity Error                             */
#define LSR_FE          (1 << 3)    /* Framing Error                            */
#define LSR_BI          (1 << 4)    /* Break Interrupt                          */
#define LSR_TDRQ        (1 << 5)    /* Transmit Data Request (THR/FIFO < half)  */
#define LSR_TEMT        (1 << 6)    /* Transmitter Empty (THR + TSR both empty) */
#define LSR_FIFOE       (1 << 7)    /* FIFO Error Status                        */

/* -----------------------------------------------------------------------
 * Clock Reset Control (APBCLOCK base 0xD4015000)
 * ----------------------------------------------------------------------- */
#define APBCLK_BASE 0xD4015000UL
#define CLK_RST_FNCLKSEL(x) ((x) << 4) /* bits [6:4] */
#define CLK_RST_RST         (1 << 2)
#define CLK_RST_FNCLK       (1 << 1)
#define CLK_RST_APBCLK      (1 << 0)

/* FNCLKSEL values */
#define FNCLKSEL_57M6       0x0     /* 57.6 MHz  */
#define FNCLKSEL_14M7       0x1     /* 14.7456 MHz */
#define FNCLKSEL_48M        0x2     /* 48 MHz    */

#define BAUD_DIV_115200 (8U)

static inline void mmio_write32(unsigned long addr, unsigned int val)
{
    *((volatile unsigned int *)addr) = val;
}

static inline unsigned int mmio_read32(unsigned long addr)
{
    return *((volatile unsigned int *)addr);
}

static inline void mmio_write8(unsigned long addr, unsigned char val)
{
    *((volatile unsigned char *)addr) = val;
}

static inline unsigned char mmio_read8(unsigned long addr)
{
    return *((volatile unsigned char *)addr);
}

static inline void mmio_write(unsigned long addr, unsigned int val)
{
#ifndef QEMU
    mmio_write32(addr, val);
#else
    mmio_write8(addr, val);
#endif // !QEMU
}

static inline unsigned int mmio_read(unsigned long addr)
{
#ifndef QEMU
    return mmio_read32(addr);
#else
    return mmio_read8(addr);
#endif // !QEMU 
}

/* -----------------------------------------------------------------------
 * Ring buffers for IRQ-driven RX/TX.
 *
 * Single-producer / single-consumer: RX is produced by the ISR and
 * consumed by uart_getc(); TX is produced by uart_putc() and drained
 * by the ISR. head == tail ⇒ empty; (head + 1) mod N == tail ⇒ full.
 * UART_BUF_SIZE must be a power of 2 so the modulo reduces to an AND.
 * ----------------------------------------------------------------------- */
#define UART_BUF_SIZE   256U
#define UART_BUF_MASK   (UART_BUF_SIZE - 1U)

struct uart_ring {
    unsigned char buf[UART_BUF_SIZE];
    volatile unsigned int head;
    volatile unsigned int tail;
};

static struct uart_ring rx_buf;
static struct uart_ring tx_buf;

static inline int ring_empty(const struct uart_ring *r)
{
    return r->head == r->tail;
}

static inline int ring_full(const struct uart_ring *r)
{
    return ((r->head + 1U) & UART_BUF_MASK) == r->tail;
}

static inline void ring_push(struct uart_ring *r, unsigned char c)
{
    unsigned int next = (r->head + 1U) & UART_BUF_MASK;
    if (next == r->tail)
        return; /* Overflow: drop newest to keep producer wait-free. */
    r->buf[r->head] = c;
    r->head = next;
}

static inline int ring_pop(struct uart_ring *r)
{
    if (r->head == r->tail)
        return -1;
    unsigned char c = r->buf[r->tail];
    r->tail = (r->tail + 1U) & UART_BUF_MASK;
    return c;
}

/** ----------------------------------------------------------------------
 * @brief uart_init() – Configure UART0 for interrupt-driven I/O.
 *
 * Enables and resets the RX/TX FIFOs, asserts MCR.OUT2 to route the
 * UART IRQ line to the PLIC, and enables RX-available interrupts.
 * TX interrupts (IER.TIE) remain off and are flipped on demand by
 * uart_putc() when bytes are queued. Baud / line format are left
 * untouched: U-Boot has already programmed them, and reprogramming
 * would drop the host-side terminal.
 * -------------------------------------------------------------------- */
void uart_init(void)
{
    /* Gate the external IRQ line on via MCR.OUT2, preserving DTR/RTS
     * that U-Boot asserted (some USB-UART bridges gate RX/TX on them).
     * OUT2 is the switch that turns "IER events" into a wire toggle
     * feeding the PLIC.                                               */
    unsigned int mcr = mmio_read(UART_BASE + UART_MCR);
    mmio_write(UART_BASE + UART_MCR, mcr | MCR_OUT2);

    /* Enable RX-data-available IRQ; keep TX IRQ off until uart_putc
     * queues data. Preserve other IER bits U-Boot may rely on.       */
    unsigned int ier = mmio_read(UART_BASE + UART_IER);
    mmio_write(UART_BASE + UART_IER, (ier & ~IER_TIE) | IER_RAVIE);
}

/** ----------------------------------------------------------------------
 * @brief uart_enable_irq_mode() – Switch getc/putc to ring-buffer path.
 *
 * Called from main() after plic_init() and sie.SEIE have been set.
 * Before this flag flips, uart_putc()/uart_getc() stay on the polled
 * path so that early boot output and any pre-PLIC prompts still work.
 * -------------------------------------------------------------------- */
void uart_enable_irq_mode(void)
{
    g_uart_irq_mode = 1;
}

/** ----------------------------------------------------------------------
 * @brief uart_top_half() – Fast-path UART ISR run in trap context.
 *
 * Drains every pending RX byte into rx_buf and pushes as many tx_buf
 * bytes as the TX FIFO accepts. Both ring transfers are bounded and
 * cheap, so they can stay in IRQ context — the heavy lift (the PLIC
 * unmask) is deferred. The PLIC claim/complete protocol guarantees
 * the same source will not re-fire until plic_complete(); by enqueuing
 * the unmask as a bottom-half task we get the lab-required "mask in
 * top half, unmask in BH" semantics for free.
 *
 * Falls back to a synchronous plic_complete() if add_task() fails so
 * an OOM cannot wedge the UART line forever.
 * @param irq IRQ id returned by plic_claim() in the trap dispatcher.
 * -------------------------------------------------------------------- */
void uart_top_half(unsigned int irq)
{
    while (mmio_read(UART_BASE + UART_LSR) & LSR_DR) {
        unsigned char ch = mmio_read(UART_BASE + UART_RBR) & 0xFF;
        ring_push(&rx_buf, ch);
    }

    while (!ring_empty(&tx_buf) &&
           (mmio_read(UART_BASE + UART_LSR) & LSR_TDRQ)) {
        int c = ring_pop(&tx_buf);
        mmio_write(UART_BASE + UART_THR, (unsigned char)c);
    }

    if (ring_empty(&tx_buf)) {
        unsigned int ier = mmio_read(UART_BASE + UART_IER);
        if (ier & IER_TIE)
            mmio_write(UART_BASE + UART_IER, ier & ~IER_TIE);
    }

    if (add_task(uart_bottom_half, (void *)(uintptr_t)irq,
                 UART_TASK_PRIO) != 0) {
        /* OOM: skip the BH and complete here so the line unmasks. */
        plic_complete(irq);
    }
}

/** ----------------------------------------------------------------------
 * @brief uart_bottom_half() – Unmask the UART PLIC source.
 *
 * Runs from task_run_pending() with sstatus.SIE = 1. The actual byte-
 * level work is consumed by uart_getc() in shell context; this BH
 * exists primarily as the unmask hook required by the spec, and as
 * the place to put any future heavier post-IRQ processing.
 * @param arg IRQ id (cast through uintptr_t) supplied by uart_top_half().
 * -------------------------------------------------------------------- */
void uart_bottom_half(void *arg)
{
    unsigned int irq = (unsigned int)(uintptr_t)arg;
    plic_complete(irq);
}

/* -----------------------------------------------------------------------
 * uart_getc() – receive one byte (blocking poll)
 *
 * Waits until the Data Ready bit (LSR.DR) is set, then reads from RBR.
 * Returns -1 on receive errors (overrun / parity / framing).
 * ----------------------------------------------------------------------- */
int uart_getc(void)
{
    int c;

    if (g_uart_irq_mode) {
        while (ring_empty(&rx_buf)) {
            schedule();
            asm volatile ("wfi");
        }
        c = ring_pop(&rx_buf);
    } else {
        c = uart_getc_raw();
    }

    return c == '\r' ? '\n' : c;
}

int uart_getc_raw(void) {
    unsigned int lsr;

    do {
        lsr = mmio_read(UART_BASE + UART_LSR);
    } while (!(lsr & LSR_DR));

    if (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI))
        return -1;

    return mmio_read(UART_BASE + UART_RBR) & 0xFF;
}


/* -----------------------------------------------------------------------
 * uart_putc() – transmit one byte (blocking poll)
 *
 * Waits until the Transmit Data Request bit (LSR.TDRQ) is set,
 * meaning the TX FIFO has room for more data, then writes the byte.
 * ----------------------------------------------------------------------- */
void uart_putc(unsigned char c)
{
    if (c == '\n')
        uart_putc('\r');

    if (g_uart_irq_mode) {
        /* Block (wfi) until the ISR has drained enough of tx_buf. */
        while (ring_full(&tx_buf)) {
            schedule();
            asm volatile ("wfi");
        }

        unsigned long sie = sie_save_clear();
        ring_push(&tx_buf, c);
        unsigned int ier = mmio_read(UART_BASE + UART_IER);
        if (!(ier & IER_TIE))
            mmio_write(UART_BASE + UART_IER, ier | IER_TIE);
        sie_restore(sie);
        return;
    }

    /* Pre-PLIC poll fallback. */
    while ((mmio_read(UART_BASE + UART_LSR) & LSR_TDRQ) == 0)
        ;

    mmio_write(UART_BASE + UART_THR, c);
}

/* -----------------------------------------------------------------------
 * uart_puts() – transmit a null-terminated string (blocking poll)
 * ----------------------------------------------------------------------- */
void uart_puts(const char *str)
{
    if (!str)
        return;
    while (*str)
        uart_putc((unsigned char)*str++);
}