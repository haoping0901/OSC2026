#include "timer.h"
#include "riscv.h"
#include "sbi.h"
#include "dtb.h"
#include "uart.h"
#include "utils.h"
#include "types.h"

#define TIMER_INTERVAL_SECS	2
#define DEFAULT_TIMEBASE_FREQ	0x989680UL	/* QEMU virt fallback */

static uint64_t g_timebase_freq;
static uint64_t g_elapsed_secs;

/** ----------------------------------------------------------------------
 * @brief read_time() – Read the current timer value.
 *
 * Executes the rdtime pseudo-instruction to obtain the current
 * value of the time CSR.  This counter increments at the platform's
 * timebase frequency.
 * @return Current 64-bit timer value.
 * -------------------------------------------------------------------- */
static inline uint64_t read_time(void)
{
	uint64_t t;

	asm volatile ("rdtime %0" : "=r"(t));
	return t;
}

/** ----------------------------------------------------------------------
 * @brief timer_init() – Enable the core timer interrupt.
 *
 * Reads the timebase frequency from the DTB /cpus node.  Falls
 * back to 10 MHz (QEMU virt default) when the property is absent.
 * Programs the first timer interrupt 2 seconds into the future,
 * then enables sie.STIE and sstatus.SIE so the interrupt can fire.
 * -------------------------------------------------------------------- */
void timer_init(void)
{
	g_timebase_freq = dtb_getprop("/cpus", "timebase-frequency");
	if (g_timebase_freq == 0)
		g_timebase_freq = DEFAULT_TIMEBASE_FREQ;

	/* Schedule the first timer interrupt. */
	sbi_set_timer(read_time() + TIMER_INTERVAL_SECS * g_timebase_freq);

	/* Enable S-mode timer interrupt (per-source switch). */
	asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_STIE));

	/* Enable global S-mode interrupts. */
	asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
}

/** ----------------------------------------------------------------------
 * @brief timer_handle_interrupt() – S-mode timer interrupt handler.
 *
 * Increments the elapsed-seconds counter by the timer interval,
 * prints the value, and reprograms the timer for the next period.
 * sbi_set_timer() implicitly clears sip.STIP per the SBI spec.
 * -------------------------------------------------------------------- */
void timer_handle_interrupt(void)
{
	g_elapsed_secs += TIMER_INTERVAL_SECS;

	uart_puts("Seconds after booting: ");
	print_dec_ulong(g_elapsed_secs);
	uart_puts("\n");

	sbi_set_timer(read_time() + TIMER_INTERVAL_SECS * g_timebase_freq);
}
