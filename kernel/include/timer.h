#ifndef __TIMER_H__
#define __TIMER_H__

#include "types.h"

/*
 * timer_init() – Enable the S-mode core timer interrupt.
 *
 * Reads the timebase-frequency from the DTB, enables both the
 * per-source (sie.STIE) and global (sstatus.SIE) interrupt switches.
 * The hardware timer itself is left un-armed until the first call
 * to add_timer(); with an empty timer queue there is nothing to fire.
 */
void timer_init(void);

/*
 * timer_top_half() – S-mode timer IRQ entry from trap dispatcher.
 *
 * Masks sie.STIE for the duration of queue mutation, drains every
 * expired timer node, hands each user callback to the bottom-half
 * task queue (so it runs with sstatus.SIE = 1 and can be preempted
 * by higher-priority device IRQs), reprograms the hardware timer for
 * the new queue head, then re-enables sie.STIE. The actual user
 * callback invocation happens later, from task_run_pending().
 */
void timer_top_half(void);

/*
 * add_timer() – Register a one-shot callback fired @sec seconds from now.
 *
 * Allocates a timer node, inserts it into the queue ordered by absolute
 * expire time, and reprograms the hardware timer when the new node
 * becomes the earliest. Non-blocking. Silently drops the request on
 * allocation failure.
 *
 * @callback: function invoked in IRQ context when the timer fires
 * @arg:      opaque pointer handed verbatim to @callback
 * @sec:      delay in whole seconds (must be > 0)
 */
void add_timer(void (*callback)(void *), void *arg, int sec);

/*
 * add_timer_us() – One-shot callback fired @usec microseconds from now.
 *
 * Same behavior as add_timer() but accepts a sub-second resolution
 * delay. Used by sys_usleep() to implement microsecond sleeps without
 * busy-waiting. @usec == 0 still queues a node that fires at the next
 * timer tick.
 */
void add_timer_us(void (*callback)(void *), void *arg, uint64_t usec);

/*
 * Tick-conversion helpers – exposed so shell callbacks can translate
 * absolute ticks back into seconds for human-readable output.
 */
uint64_t timer_get_timebase_freq(void);
uint64_t timer_read_ticks(void);

#endif /* __TIMER_H__ */
