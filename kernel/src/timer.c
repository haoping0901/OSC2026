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
#include "sched.h"

#define DEFAULT_TIMEBASE_FREQ	0x989680UL	/* QEMU virt fallback: 10 MHz */

/*
 * Hard upper bound on the next timer IRQ. Even when the user timer
 * queue is empty, we keep arming the hardware timer so timer_top_half()
 * runs periodically and can flag need_resched. With a 10 MHz timebase
 * this is ~10 ms — fine-grained enough to make U-mode preemption
 * visible, coarse enough to avoid IRQ storms.
 */
#define SCHED_TICK_DIV	32		/* timebase / 32 ≈ 31.25 ms (1/32 s) */

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
 * timer is armed for the first scheduler tick at +1/SCHED_TICK_DIV s
 * so timer_top_half() starts firing immediately after boot, even
 * before any add_timer*() caller has registered an entry.
 * -------------------------------------------------------------------- */
void timer_init(void)
{
	g_timebase_freq = dtb_getprop("/cpus", "timebase-frequency");
	if (g_timebase_freq == 0)
		g_timebase_freq = DEFAULT_TIMEBASE_FREQ;

	/* Arm the first scheduler tick (1/SCHED_TICK_DIV s) right after init. */
	sbi_set_timer(read_time() + g_timebase_freq / SCHED_TICK_DIV);

	/* Enable the per-source switch (sie.STIE). */
	asm volatile ("csrs sie, %0" :: "r"((unsigned long)SIE_STIE));

	/* Enable global S-mode interrupts. */
	asm volatile ("csrs sstatus, %0" :: "r"((unsigned long)SSTATUS_SIE));
}

/** ----------------------------------------------------------------------
 * @brief add_timer_ticks() – Internal helper shared by add_timer*().
 *
 * Allocates a node, fills it for an absolute expire time of
 * (now + @ticks), inserts it into the queue ordered by expire_tick,
 * and reprograms the hardware timer when the new node becomes the
 * earliest. Runs the queue mutation with sstatus.SIE cleared so the
 * timer IRQ handler cannot race against the walker.
 * @param     callback Function to invoke in IRQ context on expiry.
 * @param[in] arg      Opaque pointer handed to the callback verbatim.
 * @param     ticks    Delay in timebase ticks.
 * -------------------------------------------------------------------- */
static void add_timer_ticks(void (*callback)(void *), void *arg,
                            uint64_t ticks)
{
	struct timer_node *n = kmalloc(sizeof(*n));
	if (!n)
		return;

	uint64_t now = read_time();
	n->register_tick = now;
	n->expire_tick   = now + ticks;
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
 * @brief add_timer() – Register a one-shot callback after @sec seconds.
 *
 * Thin second-granularity wrapper over add_timer_ticks().
 * @param     callback Function to invoke in IRQ context on expiry.
 * @param[in] arg      Opaque pointer handed to the callback verbatim.
 * @param     sec      Delay in whole seconds.
 * -------------------------------------------------------------------- */
void add_timer(void (*callback)(void *), void *arg, int sec)
{
	add_timer_ticks(callback, arg, (uint64_t)sec * g_timebase_freq);
}

/** ----------------------------------------------------------------------
 * @brief add_timer_us() – Register a callback after @usec microseconds.
 *
 * Converts microseconds to ticks via the DTB-probed timebase frequency
 * before delegating to add_timer_ticks(). Sub-tick delays are rounded
 * down to zero ticks, in which case the callback fires on the very
 * next timer IRQ.
 * @param     callback Function to invoke in IRQ context on expiry.
 * @param[in] arg      Opaque pointer handed to the callback verbatim.
 * @param     usec     Delay in microseconds.
 * -------------------------------------------------------------------- */
void add_timer_us(void (*callback)(void *), void *arg, uint64_t usec)
{
	uint64_t ticks = g_timebase_freq * usec / 1000000ULL;
	add_timer_ticks(callback, arg, ticks);
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

	/*
	 * Always rearm with at least the scheduler tick deadline so that
	 * U-mode preemption keeps firing even when no user timers are
	 * registered. If a user timer is sooner, prefer it.
	 */
	uint64_t next = now + g_timebase_freq / SCHED_TICK_DIV;
	if (!list_empty(&g_timer_queue)) {
		struct timer_node *h = list_entry(g_timer_queue.next,
		                                  struct timer_node, link);
		if (h->expire_tick < next)
			next = h->expire_tick;
	}
	sbi_set_timer(next);

	/* Mark the current thread for preemption on the way back to U. */
	set_need_resched();
}
