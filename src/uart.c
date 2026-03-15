#include "uart.h"

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uintptr_t;
typedef unsigned long size_t;

#ifndef QEMU
#define UART_BASE 0xD4017000UL
#else
// https://github.com/qemu/qemu/blob/master/hw/riscv/virt.c#L95
#define UART_BASE 0x10000000UL
#endif // !QEMU

#ifndef QEMU
#define UART_RBR    0x00
#define UART_THR    0x00
#define UART_DLL    0x00
#define UART_DLH    0x04
#define UART_IER    0x04
#define UART_FCR    0x08
#define UART_LCR    0x0C
#define UART_LSR    0x14
#else
#define UART_RBR    0x00
#define UART_THR    0x00
#define UART_IER    0x01
#define UART_FCR    0x02
#define UART_LCR    0x03
#define UART_LSR    0x05
#endif // !QEMU

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

#if 0
static void uart_clk_enable(unsigned char fnclksel)
{
    unsigned long clk_rst_addr = APBCLK_BASE + 0x00;

    /* 1. Assert reset first (RST=1, clocks off) */
    mmio_write32(clk_rst_addr,
                 CLK_RST_FNCLKSEL(fnclksel) | CLK_RST_RST);

    /* 2. Enable APB bus clock and functional clock, keep reset asserted */
    mmio_write32(clk_rst_addr,
                 CLK_RST_FNCLKSEL(fnclksel) | CLK_RST_RST |
                 CLK_RST_FNCLK | CLK_RST_APBCLK);

    /* 3. De-assert reset (RST=0) */
    mmio_write32(clk_rst_addr,
                 CLK_RST_FNCLKSEL(fnclksel) |
                 CLK_RST_FNCLK | CLK_RST_APBCLK);
}

void uart_init(void)
{
    /* Enable clocks, de-assert reset (14.7456 MHz functional clock) */
    uart_clk_enable(FNCLKSEL_14M7);

    /* --- Program baud rate (DLAB=1) --- */
    mmio_write32(UART_BASE + UART_LCR, LCR_DLAB);
    mmio_write32(UART_BASE + UART_DLL, BAUD_DIV_115200 & 0xFF);
    mmio_write32(UART_BASE + UART_DLH, (BAUD_DIV_115200 >> 8) & 0xFF);

    /* --- Line format: 8N1 (DLAB=0) --- */
    mmio_write32(UART_BASE + UART_LCR, LCR_WLS_8 | LCR_STB_1);

    /* --- Enable and reset FIFOs (64-byte depth) --- */
    mmio_write32(UART_BASE + UART_FCR,
                 FCR_TRFIFOE | FCR_RESETRF | FCR_RESETTF | FCR_ITL_1);

    /* --- Enable UART unit, no interrupt, no DMA --- */
    mmio_write32(UART_BASE + UART_IER, IER_UUE);
}
#endif // 0

/* -----------------------------------------------------------------------
 * uart_getc() – receive one byte (blocking poll)
 *
 * Waits until the Data Ready bit (LSR.DR) is set, then reads from RBR.
 * Returns -1 on receive errors (overrun / parity / framing).
 * ----------------------------------------------------------------------- */
int uart_getc(void)
{
    unsigned int lsr;
    unsigned int ch;

    /* Poll until a character is available in the RX FIFO */
    do {
        lsr = mmio_read(UART_BASE + UART_LSR);
    } while (!(lsr & LSR_DR));

    /* Check for receive errors before reading */
    if (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI))
        return -1;

    ch = mmio_read(UART_BASE + UART_RBR) & 0xFF;
    return ch == '\r' ? '\n' : ch;
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

    /* Poll until TX FIFO has space (TDRQ=1 means <= half full) */
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