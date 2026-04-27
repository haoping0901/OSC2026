#include "timer.h"
#include "riscv.h"
#include "sbi.h"
#include "dtb.h"
#include "uart.h"
#include "utils.h"
#include "types.h"
#include "list.h"
#include "kmalloc.h"
#include "task.h"

#define DEFAULT_TIMEBASE_FREQ	0x989680UL	/* QEMU virt fallback: 10 MHz */

static uint64_t g_timebase_freq;

/*
 * Timer queue sorted by ascending expire_tick.  g_timer_queue.next is
 * always the earliest timer (= what the hardware timer is programmed
 * for).  When the queue is empty the hardware timer is parked at
 * (uint64_t)-1 so it never fires spontaneously.
 */
static struct list_head g_timer_queue = LIST_HEAD_INIT(g_timer_queue);

struct timer_node {
	struct list_head link;
	uint64_t         expire_tick;
	uint64_t         register_tick;
	void           (*cb)(void *);
	void            *arg;
};

/** ----------------------------------------------------------------------
 * @brief read_time() – Read the current value of the time CSR.
 *
 * Executes the rdtime pseudo-instruction. The counter increments at
 * the platform's timebase-frequency regardless of privilege mode.
 * @return Current 64-bit time CSR value.
 * -------------------------------------------------------------------- */
static inline uint64_t read_time(void)
{
	uint64_t t;

	asm volatile ("rdtime %0" : "=r"(t));
	return t;
}

/** ----------------------------------------------------------------------
 * @brief timer_get_timebase_freq() – Accessor for the DTB-probed freq.
 *
 * Used by callers that need to translate tick deltas into seconds
 * (e.g. the shell's setTimeout callback).
 * @return Timebase frequency in Hz.
 * -------------------------------------------------------------------- */
uint64_t timer_get_timebase_freq(void)
{
	return g_timebase_freq;
}

/** ----------------------------------------------------------------------
 * @brief timer_read_ticks() – Expose the raw time CSR to other modules.
 * @return Current 64-bit time CSR value.
 * -------------------------------------------------------------------- */
uint64_t timer_read_ticks(void)
{
	return read_time();
}

/** ----------------------------------------------------------------------
 * @brief timer_init() – Enable the S-mode core timer interrupt.
 *
 * Probes timebase-frequency from the DTB /cpus node (fallback 10 MHz
 * for QEMU virt), then enables sie.STIE and sstatus.SIE. The hardware
 * timer is pushed to the distant future — with an empty queue there
 * is nothing to fire yet, and add_timer() will reprogram it as soon
 * as the first entry is registered.
 * -------------------------------------------------------------------- */
void timer_init(void)
{
	g_timebase_freq = dtb_getprop("/cpus", "timebase-frequency");
	if (g_timebase_freq == 0)
		g_timebase_freq = DEFAULT_TIMEBASE_FREQ;

	/* Park the hardware timer: no pending timers means no IRQ wanted. */
	sbi_set_timer((uint64_t)-1);

	/* Enable the per-source switch (sie.STIE). */
	asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_STIE));

	/* Enable global S-mode interrupts. */
	asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
}

/** ----------------------------------------------------------------------
 * @brief add_timer() – Register a one-shot callback after @sec seconds.
 *
 * Inserts the new node into the queue ordered by expire_tick. If the
 * freshly inserted node becomes the earliest (queue head), the
 * hardware timer is reprogrammed via sbi_set_timer(). The whole
 * queue mutation runs with sstatus.SIE cleared to keep the timer IRQ
 * handler from racing against the walker.
 * @param callback Function to invoke in IRQ context on expiry.
 * @param arg      Opaque pointer handed to the callback verbatim.
 * @param sec      Delay in whole seconds.
 * -------------------------------------------------------------------- */
void add_timer(void (*callback)(void *), void *arg, int sec)
{
	struct timer_node *n = kmalloc(sizeof(*n));
	if (!n)
		return;

	uint64_t now = read_time();
	n->register_tick = now;
	n->expire_tick   = now + (uint64_t)sec * g_timebase_freq;
	n->cb            = callback;
	n->arg           = arg;

	unsigned long flags = sie_save_clear();

	/* Walk until we find the first node that expires *after* us. */
	struct list_head *p;
	for (p = g_timer_queue.next; p != &g_timer_queue; p = p->next) {
		struct timer_node *t = list_entry(p, struct timer_node, link);
		if (n->expire_tick < t->expire_tick)
			break;
	}
	/* Insert immediately before p (p may be the head sentinel). */
	n->link.prev = p->prev;
	n->link.next = p;
	p->prev->next = &n->link;
	p->prev = &n->link;

	/* If we just became the earliest, reprogram the hardware timer. */
	if (g_timer_queue.next == &n->link)
		sbi_set_timer(n->expire_tick);

	sie_restore(flags);
}

/** ----------------------------------------------------------------------
 * @brief timer_top_half() – Drain expired timers, hand cbs to BH queue.
 *
 * Walks g_timer_queue from the head, popping every node whose
 * expire_tick has passed. Each user callback is enqueued via
 * add_task() at TIMER_TASK_PRIO instead of being run inline so it
 * later executes from task_run_pending() with sstatus.SIE = 1 — that
 * lets a higher-priority device (UART) preempt the timer callback.
 *
 * The hardware timer is one-shot, so no explicit interrupt-line mask
 * is needed: until sbi_set_timer() rearms it at the bottom of this
 * function, no further timer IRQ can fire. add_task() failure
 * degrades to a synchronous call so a timeout is never silently lost.
 * -------------------------------------------------------------------- */
void timer_top_half(void)
{
	uint64_t now = read_time();

	while (!list_empty(&g_timer_queue)) {
		struct timer_node *t = list_entry(g_timer_queue.next,
		                                  struct timer_node, link);
		if (t->expire_tick > now)
			break;

		list_del(&t->link);
		if (add_task(t->cb, t->arg, TIMER_TASK_PRIO) != 0) {
			/* OOM fallback: keep semantics over preemption. */
			t->cb(t->arg);
		}
		kfree(t);
	}

	if (!list_empty(&g_timer_queue)) {
		struct timer_node *h = list_entry(g_timer_queue.next,
		                                  struct timer_node, link);
		sbi_set_timer(h->expire_tick);
	} else {
		sbi_set_timer((uint64_t)-1);
	}
}
